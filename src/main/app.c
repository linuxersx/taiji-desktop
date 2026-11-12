// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <stdarg.h>
#include "app.h"
#include "external_osk.h"
#include "editor.h"
#include "smb_share.h"
#include "catalog.h"
#include "desktop.h"
#include "desktop_watch.h"
#include "theme.h"
#include "i18n.h"
#include "system_locale.h"
#include "system_brightness.h"
#include "theme_auto.h"
#include "libretro.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

static void modal_app_input(App *app, InputAction action);
static void input_apps(App *app, InputAction action);
static void spawn_detached(const char *const argv[]);
static int nav_index_for_screen(App *app);
static Screen nav_screen_at(App *app, int index);
static const int k_ui_scale_steps[] = {
    100, 125, 150, 175, 200, 225, 250, 275, 300
};
#define UI_SCALE_STEP_COUNT ((int)(sizeof(k_ui_scale_steps) / sizeof(k_ui_scale_steps[0])))

int app_ui_scale_values(int *out, int cap, int max)
{
    int n = 0;
    for (int i = 0; i < UI_SCALE_STEP_COUNT; i++) {
        if (k_ui_scale_steps[i] > max) break;
        if (out && n < cap) out[n] = k_ui_scale_steps[i];
        n++;
    }
    if (out && n > cap) n = cap;
    return n;
}

static void app_apply_system_scale(App *app);
static void app_desktop_scan_start(App *app, bool notify);
static void volume_osd_hide(void);
static int app_tasks_scan(App *app, char *oldest_id, size_t oldest_size);

/* Async-signal-safe volume requests from the sway/global hotkey binding. */
static volatile sig_atomic_t g_volume_signal = 0;

static void on_volume_signal(int sig)
{
    if (sig == SIGUSR1) g_volume_signal = 1;
    else if (sig == SIGUSR2) g_volume_signal = 2;
    else if (sig == SIGHUP) g_volume_signal = 3;
}

/* Cache the current Taiji frame before a new app covers it.  This is what the
 * "Taiji" card in the multitask overview shows later. */
static void app_capture_self_preview(void)
{
    const char *env = getenv("TAIJI_CAPTURE_HELPER");
    char *path = env && *env ? xstrdup(env)
                             : xstrdup("/usr/libexec/taiji/taiji-capture");
    if (access(path, X_OK) != 0) {
        free(path);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(path, path, "--self", (char *)NULL);
        _exit(127);
    }
    free(path);
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
}

static void on_backend_input(void *userdata, InputAction action, const char *text)
{
    app_input((App *)userdata, action, text);
}

static int game_columns(App *app)
{
    return app->layout.game_cols > 0 ? app->layout.game_cols : 4;
}

static int game_page_size(App *app)
{
    return app->layout.game_page > 0 ? app->layout.game_page : 8;
}

static const char *screen_collection(App *app)
{
    if (app->screen == SCREEN_FAVORITES) return "favorites";
    if (app->screen == SCREEN_RECENT) return "recent";
    if (app->screen == SCREEN_GAMES) return app->collection_id;
    return "all";
}

void app_apply_gamepad_config(App *app)
{
    /* Custom button mapping was removed from the UI; ignore and clear any
     * legacy values so an old config cannot keep a hidden mapping active. */
    app->config.custom_mapping = false;
    app->config.map_confirm = app->config.map_cancel = -1;
    app->config.map_menu1 = app->config.map_menu2 = -1;
    backend_configure_gamepad(app->config.swap_ab, app->config.swap_xy,
                              app->config.swap_ps, false, -1, -1, -1, -1);
}

void app_save_settings(App *app)
{
    /* The OOBE keeps its choices in memory and writes one config at the end,
     * so an interrupted first-run setup is offered again next boot. */
    if (app->oobe && !app->oobe_finishing) return;
    config_save(&app->config);
    app_apply_gamepad_config(app);
    audio_set_enabled(app->config.sound);
    audio_set_volume(app->config.volume);
}

static const char *const COMMON_TIMEZONES[] = {
    "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Taipei", "Asia/Tokyo",
    "Asia/Seoul", "Asia/Singapore", "Asia/Bangkok", "Asia/Kolkata",
    "Asia/Dubai", "Asia/Jakarta", "Asia/Manila", "Asia/Kuala_Lumpur",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Moscow",
    "Europe/Rome", "Europe/Madrid",
    "America/New_York", "America/Chicago", "America/Denver",
    "America/Los_Angeles", "America/Vancouver", "America/Sao_Paulo",
    "Australia/Sydney", "Australia/Melbourne", "Pacific/Auckland",
    "UTC",
};

static int timezone_current_index(App *app)
{
    char current[128] = "";
    system_timezone_get(current, sizeof(current));
    int n = (int)(sizeof(COMMON_TIMEZONES) / sizeof(COMMON_TIMEZONES[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(current, COMMON_TIMEZONES[i]) == 0) return i;
    }
    return -1;
}

int app_timezone_option_count(App *app)
{
    int n = (int)(sizeof(COMMON_TIMEZONES) / sizeof(COMMON_TIMEZONES[0]));
    return timezone_current_index(app) >= 0 ? n : n + 1;
}

const char *app_timezone_option_id(App *app, int index)
{
    int n = (int)(sizeof(COMMON_TIMEZONES) / sizeof(COMMON_TIMEZONES[0]));
    if (index >= 0 && index < n) return COMMON_TIMEZONES[index];
    static char current[128];
    system_timezone_get(current, sizeof(current));
    return current[0] ? current : "UTC";
}

const char *app_timezone_option_label(App *app, int index)
{
    return app_timezone_option_id(app, index);
}

int app_timezone_current_option(App *app)
{
    int idx = timezone_current_index(app);
    if (idx >= 0) return idx;
    return app_timezone_option_count(app) - 1;
}

void app_set_language(App *app, int lang)
{
    lang = lang == 1 ? 1 : 0;
    app->config.language = lang;
    i18n_set_language(lang);
    char error[256] = {0};
    int scope = system_language_apply(lang, error, sizeof(error));
    app_save_settings(app);
    app_rebuild(app);

    char msg[320];
    if (lang == 1) {
        if (scope == 2)
            snprintf(msg, sizeof(msg),
                     "Language: English. System locale updated; relogin for all apps.");
        else if (scope == 1)
            snprintf(msg, sizeof(msg),
                     "Language: English. Taiji and newly launched apps use English.");
        else if (error[0] && strstr(error, "locale-gen"))
            snprintf(msg, sizeof(msg), "无法切换到英文：%s", error);
        else
            snprintf(msg, sizeof(msg), "Language: English (system locale failed: %s)",
                     error[0] ? error : "unknown");
    } else {
        if (scope == 2)
            snprintf(msg, sizeof(msg),
                     "语言已切换为中文，系统语言环境已更新；重新登录后全部应用生效。");
        else if (scope == 1)
            snprintf(msg, sizeof(msg),
                     "语言已切换为中文，太极和新启动的应用已生效。");
        else if (error[0] && strstr(error, "locale-gen"))
            snprintf(msg, sizeof(msg), "无法切换到中文：%s", error);
        else
            snprintf(msg, sizeof(msg), "语言已切换为中文（系统语言环境设置失败：%s）",
                     error[0] ? error : "未知错误");
    }
    ui_toast(app, msg);
}

void app_update_pad_labels(App *app)
{
    static int last_family = -2;
    int family = app->backend.controller ? app->backend.controller_family : -1;
    bool family_changed = family != last_family;
    last_family = family;

    const char *l = backend_trigger_left_label(&app->backend);
    const char *r = backend_trigger_right_label(&app->backend);
    bool labels_changed = strcmp(app->pad_left_label, l) != 0 ||
                          strcmp(app->pad_right_label, r) != 0;
    if (!family_changed && !labels_changed) return;
    snprintf(app->pad_left_label, sizeof(app->pad_left_label), "%s", l);
    snprintf(app->pad_right_label, sizeof(app->pad_right_label), "%s", r);
    if (app->modal == MODAL_GAMEPAD) {
        ui_build_modal(app);
        return;
    }
    if (app->modal == MODAL_NONE && !app->ui_suspended)
        app_rebuild(app);
}

void app_init(App *app)
{
    memset(app, 0, sizeof(*app));
    app->ext_osk_fd = -1;
    app->overview_close_pending = -1;
    app->pending_input_action = 0;
    app->pending_overview_focus = -1;
    app->pending_overview_activate = -1;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_volume_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    config_init(&app->config);
    {
        char cfg_path[4096];
        snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", taiji_config_dir());
        struct stat cfg_st;
        bool missing = stat(cfg_path, &cfg_st) != 0 || cfg_st.st_size == 0;
        if (!missing) {
            /* 只有注释/空白的配置等同于空配置：仍然进入初始设置。 */
            char *data = read_file(cfg_path, NULL);
            missing = !data || !config_text_has_settings(data);
            free(data);
        }
        app->config_missing = missing;
    }
    config_load(&app->config);
    app->files_show_hidden = true;
    if (app->config.language < 0 || app->config.language > 1)
        app->config.language = 0;
    i18n_set_language(app->config.language);
    system_language_export(app->config.language);
    {
        const char *skip_oobe = getenv("TAIJI_SKIP_OOBE");
        const char *force_oobe = getenv("TAIJI_OOBE");
        if ((force_oobe && *force_oobe == '1') ||
            (!(skip_oobe && *skip_oobe == '1') && app->config_missing)) {
            app->oobe = true;
            app->oobe_step = OOBE_STEP_WELCOME;
            app->oobe_focus = 0;
            app->oobe_finishing = false;
            app->oobe_rom[0] = '\0';
            if (app->config_missing) {
                /* 首次启动默认使用自动主题；强制重跑 OOBE 时保留用户主题。 */
                app->config.theme_auto = true;
                app->config.auto_dark_min = -1;
                app->config.auto_light_min = -1;
            }
            app->oobe_tz = app_timezone_current_option(app);
        }
    }
    library_init(&app->library);
    apps_init(&app->apps);
    apps_load(&app->apps);
#ifdef __linux__
    desktop_import_auto(&app->apps);
#endif
    apps_runtime_read(&app->runtime);
    app->sys.wifi = -1;
    app->sys.battery = -1;
    app->sys.charging = false;
    app->sys.battery_health = -1;
    app->sys.battery_health_label[0] = '\0';
    perf_init(&app->perf);
    gpu_init(&app->gpu);
    app->power_module = 0;
    system_volume_init(&app->sysvol);
    memset(&app->wifi, 0, sizeof(app->wifi));
    app->control_focus = 0;
    app->control_view = STATUS_WIFI;
    app->control_closing = false;
    app->status_on_bar = false;
    app->status_focus = 0;
    app->nav_on_bar = false;
    app->nav_focus = 0;
    app->control_opened_at = 0;
    app->wifi_connect_ssid[0] = '\0';
    launcher_init(&app->launcher);
    ui_init(&app->ui);
    app->screen = SCREEN_HOME;
    app->modal = MODAL_NONE;
    app->sort_mode = SORT_NAME;
    app->filter_mode = FILTER_ALL;
    app->grid_columns = app->config.grid_columns;
    app->settings_cat = 0;
    app->settings_on_rail = true;
    app->app_focus = 0;
    app->app_page = 0;
    snprintf(app->app_cat, sizeof(app->app_cat), "all");
    app->app_cat_focus = 0;
    app->app_on_categories = false;
    app_filter_apps(app);
    app->app_edit_index = -1;
    app->app_edit_row = 0;
    app->app_edit_page = 0;
    app->ui_suspended = false;
    app->external_paused = false;
    app->toast_until = 0;
    snprintf(app->collection_id, sizeof(app->collection_id), "%s",
             app->config.last_platform && *app->config.last_platform ? app->config.last_platform : "all");
    snprintf(app->selected_game_id, sizeof(app->selected_game_id), "%s",
             app->config.last_game ? app->config.last_game : "");
}

static void clamp_after_scan(App *app)
{
    Collection cols[128];
    int n = app_collections(app, cols, 128);
    if (app->home_focus >= n) app->home_focus = n > 0 ? n - 1 : 0;
    if (app->home_focus < 0) app->home_focus = 0;
    int games = 0;
    GameList list = library_query(&app->library, screen_collection(app),
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    games = list.count;
    game_list_free(&list);
    if (app->game_focus >= games) app->game_focus = games > 0 ? games - 1 : 0;
    if (app->game_focus < 0) app->game_focus = 0;
}

void app_rebuild(App *app)
{
    app_filter_apps(app);
    clamp_after_scan(app);
    ui_build(app);
    app->dirty = false;
}

static int app_theme_preset(const App *app)
{
    if (app->config.theme_auto)
        return theme_auto_is_dark(app->config.auto_dark_min,
                                  app->config.auto_light_min) ? 0 : 1;
    int preset = app->config.theme;
    if (preset < 0 || preset >= theme_preset_count()) preset = 0;
    return preset;
}

static void app_apply_theme(App *app, bool rebuild)
{
    int preset = app_theme_preset(app);
    bool changed = preset != app->active_theme;
    app->active_theme = preset;
    theme_set_preset(preset);
    if (rebuild && changed) app_rebuild(app);
}

void app_refresh(App *app)
{
    ui_update(app);
}

void app_goto_screen(App *app, Screen screen)
{
    if (app->modal != MODAL_NONE) {
        app->modal = MODAL_NONE;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
    }
    app->screen = screen;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    if (screen == SCREEN_FAVORITES) {
        snprintf(app->collection_id, sizeof(app->collection_id), "favorites");
        snprintf(app->collection_name, sizeof(app->collection_name), "我的收藏");
        app->game_focus = 0;
        app->game_page = 0;
    } else if (screen == SCREEN_RECENT) {
        snprintf(app->collection_id, sizeof(app->collection_id), "recent");
        snprintf(app->collection_name, sizeof(app->collection_name), "最近游玩");
        app->game_focus = 0;
        app->game_page = 0;
    } else if (screen == SCREEN_HOME) {
        Collection cols[128];
        int n = app_collections(app, cols, 128);
        for (int i = 0; i < n; i++) {
            if (strcmp(cols[i].id, app->collection_id) == 0) { app->home_focus = i; break; }
        }
    } else if (screen == SCREEN_APPS) {
        app->app_badge = false;
        app->apps_refresh_pending = false;
        /* Only the first visit starts at "all" / the first app. Switching away
         * with L/R and coming back must keep the category and focus, otherwise
         * the page appears to spring back. */
        if (!app->apps_visited) {
            app->apps_visited = true;
            snprintf(app->app_cat, sizeof(app->app_cat), "all");
            app->app_cat_focus = 0;
            app->app_focus = 0;
            app->app_focus_want = 0;
            app->app_page = 0;
            app->app_reset_focus = true;
        }
#ifdef __linux__
        app_desktop_scan_start(app, false);
#endif
        app_filter_apps(app);
        app->app_on_categories = false;
        apps_runtime_read(&app->runtime);
    } else if (screen == SCREEN_STORE) {
        app->store_on_sidebar = false;
        app->store_view_row = 0;
    } else if (screen == SCREEN_FILES) {
        app_files_enter(app);
    } else if (screen == SCREEN_SETTINGS) {
        app->settings_on_rail = true;
        app->settings_about = false;
        app->settings_license = false;
        app->settings_cat = 5;
        app->settings_row = 0;
        app->settings_page = 0;
    }
    app_rebuild(app);
}

int app_collections(App *app, Collection *out, int max)
{
    int n = 0;
    if (app->library.game_count <= 0) return 0;
    if (n < max) {
        Collection *c = &out[n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "all");
        snprintf(c->name, sizeof(c->name), "全部游戏");
        snprintf(c->short_name, sizeof(c->short_name), "ALL GAMES");
        char *logo = catalog_logo_path("all");
        snprintf(c->logo, sizeof(c->logo), "%s", logo);
        free(logo);
        c->count = app->library.game_count;
        c->accent = PAL.accent;
        for (int i = 0; i < app->library.game_count; i++) {
            const char *cp = app->library.games[i].cover_path;
            if (cp && cp[0]) {
                snprintf(c->cover, sizeof(c->cover), "%s", cp);
                break;
            }
        }
    }
    int recent = 0, favs = 0;
    const char *recent_cover = NULL, *fav_cover = NULL;
    int64_t latest = 0;
    for (int i = 0; i < app->library.game_count; i++) {
        Game *g = &app->library.games[i];
        if (g->last_played > 0) {
            recent++;
            if (g->last_played >= latest) { latest = g->last_played; recent_cover = g->cover_path; }
        }
        if (g->favorite) {
            favs++;
            if (!fav_cover && g->cover_path && g->cover_path[0]) fav_cover = g->cover_path;
        }
    }
    if (recent > 0 && n < max) {
        Collection *c = &out[n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "recent");
        snprintf(c->name, sizeof(c->name), "最近游玩");
        snprintf(c->short_name, sizeof(c->short_name), "RECENTLY PLAYED");
        c->count = recent;
        c->accent = PAL.accent2;
        if (recent_cover) snprintf(c->cover, sizeof(c->cover), "%s", recent_cover);
    }
    if (favs > 0 && n < max) {
        Collection *c = &out[n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "favorites");
        snprintf(c->name, sizeof(c->name), "我的收藏");
        snprintf(c->short_name, sizeof(c->short_name), "FAVORITES");
        c->count = favs;
        c->accent = 0xFFFF6B9A;
        if (fav_cover) snprintf(c->cover, sizeof(c->cover), "%s", fav_cover);
    }
    for (int i = 0; i < app->library.platform_count && n < max; i++) {
        Platform *p = &app->library.platforms[i];
        if (p->game_count <= 0) continue;
        Collection *c = &out[n++];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "%s", p->id);
        snprintf(c->name, sizeof(c->name), "%s", catalog_zh_name(p->id));
        const PlatformDef *def = catalog_by_id(p->id);
        snprintf(c->short_name, sizeof(c->short_name), "%s", def ? def->short_name : "");
        if (p->logo) snprintf(c->logo, sizeof(c->logo), "%s", p->logo);
        if (p->cover_path) snprintf(c->cover, sizeof(c->cover), "%s", p->cover_path);
        c->count = p->game_count;
        c->accent = platform_accent(p->id);
    }
    return n;
}

int app_total_collections(App *app)
{
    Collection cols[128];
    return app_collections(app, cols, 128);
}

bool app_current_collection(App *app, Collection *out)
{
    Collection cols[128];
    int n = app_collections(app, cols, 128);
    const char *id = screen_collection(app);
    for (int i = 0; i < n; i++) {
        if (strcmp(cols[i].id, id) == 0) { *out = cols[i]; return true; }
    }
    return false;
}

Game *app_selected_game(App *app)
{
    GameList list = library_query(&app->library, screen_collection(app),
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    if (list.count == 0) { game_list_free(&list); return NULL; }
    int idx = app->game_focus;
    if (idx < 0) idx = 0;
    if (idx >= list.count) idx = list.count - 1;
    Game *g = list.items[idx];
    game_list_free(&list);
    return g;
}

void app_enter_collection(App *app, const char *id)
{
    if (!id) return;
    if (strcmp(id, "favorites") == 0) { app_goto_screen(app, SCREEN_FAVORITES); return; }
    if (strcmp(id, "recent") == 0) { app_goto_screen(app, SCREEN_RECENT); return; }
    app->screen = SCREEN_GAMES;
    snprintf(app->collection_id, sizeof(app->collection_id), "%s", id);
    Platform *p = library_find_platform(&app->library, id);
    if (p) snprintf(app->collection_name, sizeof(app->collection_name), "%s", catalog_zh_name(p->id));
    else if (strcmp(id, "all") == 0) snprintf(app->collection_name, sizeof(app->collection_name), "全部游戏");
    app->game_focus = 0;
    app->game_page = 0;
    free(app->config.last_platform);
    app->config.last_platform = xstrdup(id);
    config_save(&app->config);
    app_rebuild(app);
}

void app_open_game(App *app, const char *game_id)
{
    Game *g = library_find_game(&app->library, game_id);
    if (!g) return;
    snprintf(app->collection_id, sizeof(app->collection_id), "%s", g->platform_id);
    app->screen = SCREEN_GAMES;
    Platform *p = library_find_platform(&app->library, g->platform_id);
    if (p) snprintf(app->collection_name, sizeof(app->collection_name), "%s", catalog_zh_name(p->id));
    GameList list = library_query(&app->library, g->platform_id,
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    app->game_focus = 0;
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.items[i]->id, game_id) == 0) { app->game_focus = i; break; }
    }
    game_list_free(&list);
    app->game_page = app->game_focus / game_page_size(app);
    snprintf(app->selected_game_id, sizeof(app->selected_game_id), "%s", game_id);
    app->modal = MODAL_NONE;
    ui_close_modal_visual(app);
    app_rebuild(app);
}

void app_filter_apps(App *app)
{
    app->app_filter_count = 0;
    if (!app->app_cat[0]) snprintf(app->app_cat, sizeof(app->app_cat), "all");
    for (int i = 0; i < app->apps.count; i++) {
        if (apps_category_matches(&app->apps.apps[i], app->app_cat)) {
            app->app_filter[app->app_filter_count++] = i;
        }
    }
    /* Keep the focus across an empty category. Clamping it to 0 here made
     * L2/R2 return to the first app when a category had no entries. */
    if (app->app_filter_count > 0 && app->app_focus >= app->app_filter_count)
        app->app_focus = app->app_filter_count - 1;
    if (app->app_filter_count == 0) app->app_focus = 0;
    if (app->app_focus < 0) app->app_focus = 0;
}

int app_selected_app_index(App *app)
{
    if (app->app_focus < 0 || app->app_focus >= app->app_filter_count) return -1;
    return app->app_filter[app->app_focus];
}

AppEntry *app_selected_entry(App *app)
{
    int idx = app_selected_app_index(app);
    if (idx < 0 || idx >= app->apps.count) return NULL;
    return &app->apps.apps[idx];
}

void app_set_app_category(App *app, const char *id)
{
    snprintf(app->app_cat, sizeof(app->app_cat), "%s", id && *id ? id : "all");
    app->app_cat_focus = apps_category_index(app->app_cat);
    /* app_focus_want is never clamped by a smaller category, so returning to
     * a larger one restores the position the user left. */
    int want = app->app_focus_want;
    app_filter_apps(app);
    if (app->app_filter_count > 0) {
        app->app_focus = want < app->app_filter_count
                             ? want : app->app_filter_count - 1;
    } else {
        app->app_focus = 0;
    }
    app->app_on_categories = false;
    taiji_debug_log("category=%s focus=%d want=%d count=%d", app->app_cat,
                    app->app_focus, app->app_focus_want,
                    app->app_filter_count);
    app_rebuild(app);
}

/* ── Launching ────────────────────────────────────────────────────────────── */

static int core_item_add(App *app, CoreItemKind kind, const char *label,
                         const char *core, const char *path, bool compatible)
{
    if (app->core_item_count >= 64) return -1;
    CoreChoiceItem *it = &app->core_items[app->core_item_count];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    snprintf(it->label, sizeof(it->label), "%s", label ? label : "");
    if (core) snprintf(it->core, sizeof(it->core), "%s", core);
    if (path) snprintf(it->path, sizeof(it->path), "%s", path);
    it->compatible = compatible;
    return app->core_item_count++;
}

static void command_core_name(const char *cmd, char *out, size_t out_size)
{
    out[0] = '\0';
    char path[512];
    if (!cmd || !*cmd || !libretro_command_core(cmd, path, sizeof(path))) return;
    const char *base = path_basename(path);
    size_t len = strlen(base);
    static const char suffix[] = "_libretro.so";
    size_t suffix_len = sizeof(suffix) - 1;
    if (len > suffix_len && strcmp(base + len - suffix_len, suffix) == 0)
        len -= suffix_len;
    snprintf(out, out_size, "%.*s", (int)len, base);
}

static int core_find_name(App *app, const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < app->core_item_count; i++) {
        if (app->core_items[i].kind == CORE_ITEM_CORE &&
            strcmp(app->core_items[i].core, name) == 0)
            return i;
    }
    return -1;
}

static void core_items_build(App *app, bool all)
{
    app->core_item_count = 0;
    app->core_picker_all = all;

    LibretroCore cores[64];
    if (all) {
        int n = libretro_scan(cores, 64);
        LibretroCore comp[64];
        int nc = libretro_compatible(app->core_platform_id, comp, 64);
        if (n == 0) {
            core_item_add(app, CORE_ITEM_MANUAL, "手动输入命令…", NULL, NULL, false);
            core_item_add(app, CORE_ITEM_BACK, "返回兼容核心列表", NULL, NULL, false);
            return;
        }
        for (int i = 0; i < n; i++) {
            bool compat = false;
            bool rec = false;
            for (int j = 0; j < nc; j++) {
                if (strcmp(comp[j].name, cores[i].name) != 0) continue;
                compat = true;
                rec = comp[j].recommended;
                break;
            }
            char label[160];
            if (rec)
                snprintf(label, sizeof(label), "%s（推荐）", cores[i].label);
            else if (compat)
                snprintf(label, sizeof(label), "%s（兼容）", cores[i].label);
            else
                snprintf(label, sizeof(label), "%s（未验证）", cores[i].label);
            core_item_add(app, CORE_ITEM_CORE, label, cores[i].name,
                          cores[i].path, compat);
        }
        core_item_add(app, CORE_ITEM_BACK, "返回兼容核心列表", NULL, NULL, false);
        return;
    }

    int n = libretro_compatible(app->core_platform_id, cores, 64);
    if (n == 0) {
        core_item_add(app, CORE_ITEM_ALL, "选择其他核心…", NULL, NULL, false);
        core_item_add(app, CORE_ITEM_MANUAL, "手动输入命令…", NULL, NULL, false);
        return;
    }

    char label[192];
    snprintf(label, sizeof(label), "自动（推荐：%s）", cores[0].label);
    core_item_add(app, CORE_ITEM_AUTO, label, NULL, NULL, true);
    for (int i = 0; i < n; i++) {
        if (cores[i].recommended)
            snprintf(label, sizeof(label), "%s（推荐）", cores[i].label);
        else
            snprintf(label, sizeof(label), "%s", cores[i].label);
        core_item_add(app, CORE_ITEM_CORE, label, cores[i].name,
                      cores[i].path, true);
    }
    core_item_add(app, CORE_ITEM_ALL, "全部已安装核心…", NULL, NULL, false);
    if (!app->core_picker_launch)
        core_item_add(app, CORE_ITEM_MANUAL, "手动输入命令…", NULL, NULL, false);
}

static int core_focus_for_current(App *app)
{
    const char *cmd = config_command_for(&app->config, app->core_platform_id);
    char cur[64];
    command_core_name(cmd, cur, sizeof(cur));
    if (cur[0]) {
        int idx = core_find_name(app, cur);
        if (idx >= 0) return idx;
    }
    if (!app->core_picker_all) {
        if (!cmd || !*cmd) {
            for (int i = 0; i < app->core_item_count; i++)
                if (app->core_items[i].kind == CORE_ITEM_AUTO) return i;
        } else if (!cur[0]) {
            for (int i = 0; i < app->core_item_count; i++)
                if (app->core_items[i].kind == CORE_ITEM_MANUAL) return i;
        }
    }
    for (int i = 0; i < app->core_item_count; i++)
        if (app->core_items[i].kind == CORE_ITEM_CORE) return i;
    return 0;
}

static void core_picker_set_title(App *app, const char *base)
{
    bool no_compat = !app->core_picker_all && app->core_item_count > 0 &&
                     app->core_items[0].kind == CORE_ITEM_ALL;
    snprintf(app->modal_title, sizeof(app->modal_title), "%s",
             no_compat ? "未找到兼容核心" : base);
}

static void app_open_core_picker(App *app, const char *platform_id,
                                 bool for_launch, const char *game_id)
{
    if (!platform_id || !*platform_id) return;
    app->modal = MODAL_CHOICE;
    app->choice_purpose = CHOICE_CORE;
    snprintf(app->core_platform_id, sizeof(app->core_platform_id), "%s", platform_id);
    app->core_picker_launch = for_launch && game_id && *game_id;
    snprintf(app->core_pending_game_id, sizeof(app->core_pending_game_id), "%s",
             app->core_picker_launch ? game_id : "");
    /* Pick up cores installed since Taiji started. */
    libretro_refresh();
    core_items_build(app, false);
    app->choice_focus = core_focus_for_current(app);
    core_picker_set_title(app, "选择启动核心");
    app->modal_hint[0] = '\0';
    ui_build_modal(app);
}

static void core_picker_show_all(App *app)
{
    core_items_build(app, true);
    app->choice_focus = core_focus_for_current(app);
    snprintf(app->modal_title, sizeof(app->modal_title), "%s", "全部已安装核心");
    ui_refresh_modal(app);
}

static void core_picker_show_compatible(App *app)
{
    core_items_build(app, false);
    app->choice_focus = core_focus_for_current(app);
    core_picker_set_title(app, "选择启动核心");
    ui_refresh_modal(app);
}

static void app_launch_game_now(App *app, Game *g, const char *override_cmd)
{
    if (!g) return;
    if (app->launcher.active) {
        if (app->launcher.external && app->launcher.game_session &&
            strcmp(app->active_game_id, g->id) == 0) {
            apps_runtime_read(&app->runtime);
            if (strcmp(app->runtime.app_state, "paused") == 0 ||
                strcmp(app->runtime.foreground, "taiji") == 0) {
                app_resume_external(app);
                return;
            }
        }
        ui_toast(app, "已有游戏正在运行");
        return;
    }
    app_capture_self_preview();
    Platform *p = library_find_platform(&app->library, g->platform_id);
    char *error = NULL;
    bool session = launcher_session_available();
    bool use_app_launcher = session || (override_cmd && *override_cmd);
    if (use_app_launcher) {
        char *cmd;
        if (override_cmd && *override_cmd) {
            Game tmp = *g;
            tmp.command = (char *)override_cmd;
            cmd = launcher_build_command(&app->config, p, &tmp);
        } else {
            cmd = launcher_build_command(&app->config, p, g);
        }
        AppEntry tmp;
        memset(&tmp, 0, sizeof(tmp));
        snprintf(tmp.id, sizeof(tmp.id), "game:%s", g->id);
        snprintf(tmp.name, sizeof(tmp.name), "%s", g->name);
        snprintf(tmp.command, sizeof(tmp.command), "%s", cmd);
        free(cmd);
        tmp.mode = APP_MODE_RESIDENT;
        tmp.on_return = APP_ON_RETURN_PAUSE;
        tmp.heavy = true;
        if (!launcher_start_app(&app->launcher, &tmp, &error)) {
            ui_toast(app, error ? error : "启动失败");
            free(error);
            audio_play(SND_BACK);
            return;
        }
        app->launcher.game_session = true;
        if (session) app_set_ui_suspended(app, true);
    } else if (!launcher_start(&app->launcher, &app->config, p, g, &error)) {
        ui_toast(app, error ? error : "启动失败");
        free(error);
        audio_play(SND_BACK);
        return;
    }
    snprintf(app->active_game_id, sizeof(app->active_game_id), "%s", g->id);
    app->launch_started = (int64_t)time(NULL);
    free(app->config.last_game);
    app->config.last_game = xstrdup(g->id);
    config_save(&app->config);
    audio_play(SND_ENTER);
    char msg[512];
    snprintf(msg, sizeof(msg), "正在启动 %s", g->name);
    ui_toast(app, msg);
    if (!session) SDL_MinimizeWindow(app->backend.window);
}

void app_launch_game(App *app, Game *game)
{
    Game *g = game;
    if (!g) g = app_selected_game(app);
    if (!g) return;

#ifndef __APPLE__
    const char *saved_cmd = config_command_for(&app->config, g->platform_id);
    bool stale_core = false;
    if (saved_cmd && *saved_cmd) {
        char core_path[512];
        if (libretro_command_core(saved_cmd, core_path, sizeof(core_path))) {
            if (!file_exists(core_path)) stale_core = true;
        } else if (strstr(saved_cmd, "retroarch")) {
            /* A bare "retroarch %ROM%" cannot work with dynamic cores. */
            stale_core = true;
        }
    }
    bool auto_no_core = false;
    if (saved_cmd && !*saved_cmd) {
        char *def = catalog_default_command(g->platform_id, false);
        bool retroarch = def && (strncmp(def, "retroarch", 9) == 0 ||
                                 strncmp(def, "xdg-open", 8) == 0);
        free(def);
        if (retroarch) {
            LibretroCore probe[1];
            auto_no_core = libretro_compatible(g->platform_id, probe, 1) == 0;
        }
    }
    if (!app->launcher.active && (!g->command || !g->command[0]) &&
        (!saved_cmd || stale_core || auto_no_core)) {
        char *def = catalog_default_command(g->platform_id, false);
        bool retroarch = def && strncmp(def, "retroarch", 9) == 0;
        bool no_emulator = def && strncmp(def, "xdg-open", 8) == 0;
        free(def);
        if (stale_core && !retroarch && !no_emulator) {
            /* The saved core disappeared, but a standalone emulator is now
             * available. Drop the stale command and use the new default. */
            config_remove_command(&app->config, g->platform_id);
            config_save(&app->config);
            app_launch_game_now(app, g, NULL);
            return;
        }
        if (retroarch || no_emulator) {
            LibretroCore cores[64];
            int n = libretro_compatible(g->platform_id, cores, 64);
            if (n == 1) {
                char *quoted = shell_quote(cores[0].path);
                char *cmd = xasprintf("retroarch -L %s %%ROM%%", quoted);
                free(quoted);
                if (stale_core) {
                    config_set_command(&app->config, g->platform_id, cmd);
                    config_save(&app->config);
                }
                app_launch_game_now(app, g, cmd);
                free(cmd);
            } else {
                /* Zero compatible cores: explain and offer other cores/manual.
                 * Several: let the user choose before the first launch. */
                app_open_core_picker(app, g->platform_id, true, g->id);
            }
            return;
        }
    }
#endif

    app_launch_game_now(app, g, NULL);
}

void app_toggle_favorite(App *app, Game *game)
{
    if (!game) game = app_selected_game(app);
    if (!game) return;
    library_toggle_favorite(&app->library, game);
    ui_toast(app, game->favorite ? "已加入收藏" : "已取消收藏");
    audio_play(SND_NAV_STRONG);
    if (app->screen == SCREEN_FAVORITES) app_rebuild(app);
    else ui_update(app);
}

/* ── External app launch / foreground state ─────────────────────────────── */

static bool session_mode(void)
{
    const char *mode = getenv("TAIJI_SESSION_MODE");
    return mode && *mode;
}

void app_set_ui_suspended(App *app, bool suspended)
{
    if (app->ui_suspended == suspended) return;
    app->ui_suspended = suspended;
    fprintf(stderr, "[taiji] ui %s\n", suspended ? "suspended" : "resumed");
    if (suspended) {
        audio_shutdown();
        if (!session_mode()) SDL_MinimizeWindow(app->backend.window);
    } else {
        volume_osd_hide();
        audio_init();
        audio_set_enabled(app->config.sound);
        audio_set_volume(app->config.volume);
        if (!session_mode()) {
            SDL_RestoreWindow(app->backend.window);
            SDL_RaiseWindow(app->backend.window);
        }
        SDL_ShowCursor(SDL_ENABLE);
        backend_refresh_size(&app->backend);
        app->backend.resized = false;
        app_rebuild(app);
    }
}

void app_refresh_runtime(App *app)
{
    char *path = apps_runtime_path();
    bool has_state = file_exists(path);
    free(path);
    if (!has_state) return;
    apps_runtime_read(&app->runtime);
    if (strcmp(app->runtime.foreground, "taiji") == 0) {
        if (app->ui_suspended) app_set_ui_suspended(app, false);
    } else if (strcmp(app->runtime.foreground, "app") == 0 ||
               strcmp(app->runtime.foreground, "music") == 0) {
        if (!app->ui_suspended) app_set_ui_suspended(app, true);
    }
}

static char *runtime_file(const char *name)
{
    char *state = apps_runtime_path();
    char *dir = path_dirname_dup(state);
    mkdir_p(dir);
    char *path = path_join(dir, name);
    free(state);
    free(dir);
    return path;
}

static bool request_handoff(App *app, AppEntry *entry)
{
    char *helper = getenv("TAIJI_APP_HELPER");
    char *hpath = helper && *helper ? xstrdup(helper) : xstrdup("/usr/libexec/taiji/taiji-run-app");
    if (access(hpath, X_OK) != 0) {
        free(hpath);
        return false;
    }
    char *qid = shell_quote(entry->id);
    char *qret = shell_quote(apps_return_name(entry->on_return));
    char *qcmd = shell_quote(entry->command);
    char *script = xasprintf("#!/bin/sh\nexec %s %s handoff %s %s\n",
                             hpath, qid, qret, qcmd);
    char *pending = runtime_file("handoff");
    bool ok = write_file_atomic(pending, script, strlen(script));
    if (ok) chmod(pending, 0700);
    free(pending);
    free(script);
    free(qid); free(qret); free(qcmd); free(hpath);
    if (!ok) return false;

    char *resume = runtime_file("resume_screen");
    const char *screen = app->screen == SCREEN_APPS ? "apps" :
                         app->screen == SCREEN_SETTINGS ? "settings" : "home";
    write_file_atomic(resume, screen, strlen(screen));
    free(resume);

    app->exit_code = 77;
    app->backend.running = false;
    return true;
}

static void launch_external_entry(App *app, AppEntry *entry)
{
    app_capture_self_preview();
    bool handoff = entry->mode == APP_MODE_HANDOFF && !entry->background;
    if (entry->mode == APP_MODE_AUTO && !entry->background) {
        if (app->config.app_mode == 2) handoff = true;
        else if (app->config.app_mode == 1) handoff = false;
        else handoff = app->config.low_power && entry->heavy;
    }
    if (handoff && session_mode() && launcher_session_available()) {
        if (request_handoff(app, entry)) {
            audio_play(SND_ENTER);
            return;
        }
        ui_toast(app, "接管模式启动失败，改为常驻模式");
    }

    char *error = NULL;
    if (!launcher_start_app(&app->launcher, entry, &error)) {
        ui_toast(app, error ? error : "启动失败");
        free(error);
        audio_play(SND_BACK);
        return;
    }
    snprintf(app->active_app_id, sizeof(app->active_app_id), "%s", entry->id);
    app->launch_started = (int64_t)time(NULL);
    audio_play(SND_ENTER);
    char msg[512];
    if (entry->background) {
        snprintf(msg, sizeof(msg), "已后台启动 %s", entry->name);
        ui_toast(app, msg);
        return;
    }
    app_set_ui_suspended(app, true);
    snprintf(msg, sizeof(msg), "正在启动 %s", entry->name);
    ui_toast(app, msg);
}

void app_launch_external(App *app)
{
    if (app->apps.count == 0) {
        char hint[64];
        ui_button_hint(app, hint, sizeof(hint), "还没有应用，按 {Y} 添加");
        ui_toast(app, hint);
        return;
    }
    AppEntry *entry = app_selected_entry(app);
    if (!entry) return;

    char task_state[32];
    if (session_mode() && app_task_state(app, entry->id, task_state, sizeof(task_state))) {
        app_resume_app(app, entry->id);
        return;
    }

    apps_runtime_read(&app->runtime);
    if (!session_mode()) {
        if (app->launcher.active && app->launcher.external &&
            strcmp(app->launcher.game_id ? app->launcher.game_id : "", entry->id) == 0) {
            if (strcmp(app->runtime.app_state, "paused") == 0 ||
                strcmp(app->runtime.foreground, "taiji") == 0) {
                app_resume_external(app);
            } else {
                ui_toast(app, "应用已经在前台");
            }
            return;
        }
        if (app->launcher.active) {
            ui_toast(app, "已有程序正在运行，请先返回太极");
            return;
        }
    } else {
        if (app->launcher.active && app->launcher.game_session) {
            ui_toast(app, "游戏正在运行，请先返回太极");
            return;
        }
        int max = app->config.max_paused_apps;
        if (max < 1) max = 1;
        if (max > 6) max = 6;
        char oldest[80] = {0};
        int count = app_tasks_scan(app, oldest, sizeof(oldest));
        if (count >= max) {
            snprintf(app->pending_app_id, sizeof(app->pending_app_id), "%s", entry->id);
            snprintf(app->pending_evict_id, sizeof(app->pending_evict_id), "%s",
                     oldest[0] ? oldest : "");
            char body[512];
            snprintf(body, sizeof(body),
                     "最多暂停 %d 个应用。\n是否关闭最久未使用的应用并继续启动「%s」？",
                     max, entry->name);
            app_open_confirm(app, CONFIRM_PAUSE_LIMIT, 0,
                             "暂停应用已达上限", body);
            return;
        }
    }

    launch_external_entry(app, entry);
}

void app_resume_app(App *app, const char *id)
{
    if (!id || !*id) return;
    const char *helper = getenv("TAIJI_RESUME_HELPER");
    char *path = helper && *helper ? xstrdup(helper)
                                   : xstrdup("/usr/libexec/taiji/taiji-resume");
    if (access(path, X_OK) != 0) {
        free(path);
        ui_toast(app, "恢复脚本未安装");
        return;
    }
    const char *argv[] = {path, id, NULL};
    spawn_detached(argv);
    free(path);
    app_set_ui_suspended(app, true);
    ui_toast(app, "正在恢复应用");
}

void app_resume_external(App *app)
{
    apps_runtime_read(&app->runtime);
    if (!app->runtime.app_id[0]) {
        ui_toast(app, "没有可恢复的应用");
        return;
    }
    app_resume_app(app, app->runtime.app_id);
}

void app_open_app_editor(App *app, int index)
{
    if (index < 0 || index >= app->apps.count) return;
    app->app_edit_index = index;
    app->app_edit_row = 0;
    app->app_edit_page = 0;
    app->modal = MODAL_APP;
    ui_set_input_active(false);
    ui_build_modal(app);
}

/* ── Control center ──────────────────────────────────────────────────────── */

static void app_do_wifi_connect(App *app, const char *ssid, const char *password)
{
    char error[256] = {0};
    bool ok = network_wifi_connect(ssid, password, error, sizeof(error));
    network_wifi_scan_async(false);
    backend_set_refresh_limit(&app->backend, app->config.refresh_hz);
    backend_set_render_scale(&app->backend, app->config.render_scale);

    system_status_poll(&app->sys);
    ui_update_status(app);
    app->control_focus = 0;
    app->modal = MODAL_CONTROL;
    ui_build_modal(app);
    if (ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "已连接 Wi-Fi：%s", ssid);
        ui_toast(app, msg);
    } else {
        ui_toast(app, error[0] ? error : "Wi-Fi 连接失败");
    }
}

static int mode_index_of(const bool modes[3], int mode)
{
    int idx = 0;
    for (int i = 0; i < 3; i++) {
        if (!modes[i]) continue;
        if (i == mode) return idx;
        idx++;
    }
    return 0;
}

void app_open_control_center_view(App *app, int view)
{
    if (app->ui_suspended) return;
    app->control_osd = false;
    if (view == STATUS_WIFI) {
        /* Detect both radio switches first, then scan asynchronously so the
         * panel opens immediately even when Bluetooth/Wi-Fi scans are slow. */
        bool w_enabled = app->wifi.enabled;
        bool w_available = app->wifi.available;
        network_wifi_radio(&w_enabled, &w_available);
        app->wifi.enabled = w_enabled;
        app->wifi.available = w_available;
        bluetooth_radio(&app->bt);
        if (app->wifi.count == 0) {
            snprintf(app->wifi.message, sizeof(app->wifi.message), "正在扫描 Wi-Fi...");
        }
        if (app->bt.count == 0) {
            snprintf(app->bt.message, sizeof(app->bt.message), "正在扫描蓝牙...");
        }
        network_wifi_scan_async(false);
        /* 只有列表为空时才真正 discovery；否则先显示 BlueZ 缓存，避免每次
         * 打开面板都等待 7 秒扫描。 */
        bluetooth_scan_async(app->bt.count == 0);
        app->control_radio = 0;
        app->control_col = 0;
        app->control_focus = 0;
        app->control_expanded = true;
    }
    if (view == STATUS_VOLUME) system_volume_poll(&app->sysvol);
    app->control_view = view;
    app->control_focus = 0;
    if (view == STATUS_BATTERY) {
        if (app->power_module == 0 && !app->perf.available && app->gpu.available)
            app->power_module = 1;
        if (app->power_module == 1 && !app->gpu.available && app->perf.available)
            app->power_module = 0;
        if (app->power_module == 1 && app->gpu.available)
            app->control_focus = mode_index_of(app->gpu.modes, app->gpu.mode);
        else if (app->perf.available)
            app->control_focus = mode_index_of(app->perf.modes, app->perf.mode);
    }
    app->status_on_bar = false;
    app->control_opened_at = lv_tick_get();
    app->modal = MODAL_CONTROL;
    ui_set_input_active(false);
    ui_build_modal(app);
}

static void spawn_power_action(const char *action)
{
    const char *helper = getenv("TAIJI_POWER_HELPER");
    char *path = helper && *helper ? xstrdup(helper)
                                   : xstrdup("/usr/libexec/taiji/taiji-power");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (access(path, X_OK) == 0)
            execl(path, path, action, (char *)NULL);
#ifdef __APPLE__
        const char *cmd = NULL;
        if (strcmp(action, "sleep") == 0) cmd = "pmset sleepnow";
        else if (strcmp(action, "reboot") == 0)
            cmd = "osascript -e 'tell application \"System Events\" to restart'";
        else if (strcmp(action, "poweroff") == 0)
            cmd = "osascript -e 'tell application \"System Events\" to shut down'";
        else if (strcmp(action, "logout") == 0)
            cmd = "osascript -e 'tell application \"System Events\" to log out'";
#else
        const char *cmd = NULL;
        if (strcmp(action, "sleep") == 0) cmd = "loginctl suspend || systemctl suspend";
        else if (strcmp(action, "reboot") == 0) cmd = "systemctl reboot";
        else if (strcmp(action, "poweroff") == 0) cmd = "systemctl poweroff";
        else if (strcmp(action, "logout") == 0)
            cmd = "loginctl terminate-session \"${XDG_SESSION_ID:-}\" || systemctl --user exit";
#endif
        if (cmd) execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    free(path);
}

void app_open_power_menu(App *app)
{
    if (app->modal != MODAL_NONE) app_close_modal(app);
    app->power_focus = 0;
    app->modal = MODAL_POWER;
    ui_build_modal(app);
}

void app_power_action(App *app, int action)
{
    static const char *names[4] = {"sleep", "reboot", "poweroff", "logout"};
    if (action < 0 || action >= 4) return;
    app_close_modal(app);
    spawn_power_action(names[action]);
}

static bool power_request_pending(char *path, size_t path_size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
        snprintf(path, path_size, "%s/taiji/power_request", runtime);
    else
        snprintf(path, path_size, "/tmp/taiji/power_request");
    return access(path, F_OK) == 0;
}

static void session_request_path(const char *name, bool targeted,
                                 char *path, size_t path_size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *base = (runtime && *runtime) ? runtime : "/tmp";
    if (targeted)
        snprintf(path, path_size, "%s/taiji/%s.%ld", base, name, (long)getpid());
    else
        snprintf(path, path_size, "%s/taiji/%s", base, name);
}

static bool session_request_pending(const char *name, char *path, size_t path_size)
{
    /* Targeted request first: the session scripts address the Taiji window
     * they just focused, so a second instance cannot swallow the request. */
    session_request_path(name, true, path, path_size);
    if (access(path, F_OK) == 0) return true;
    session_request_path(name, false, path, path_size);
    return access(path, F_OK) == 0;
}

static void session_request_clear(const char *name)
{
    char path[512];
    session_request_path(name, true, path, sizeof(path));
    unlink(path);
    session_request_path(name, false, path, sizeof(path));
    unlink(path);
}

/* ── Multitask overview ──────────────────────────────────────────────────── */

static void spawn_detached(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
}

/* ── External volume OSD ────────────────────────────────────────────────── */

static bool osd_socket_path(char *path, size_t size)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return false;
    snprintf(path, size, "%s/taiji/osd.sock", rt);
    return true;
}

static bool osd_send(const char *msg)
{
    char path[PATH_MAX];
    if (!osd_socket_path(path, sizeof(path))) return false;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) plen = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path, plen);
    addr.sun_path[plen] = '\0';
    ssize_t n = sendto(fd, msg, strlen(msg), 0,
                       (struct sockaddr *)&addr, sizeof(addr));
    if (n < 0) fprintf(stderr, "[taiji] osd send failed: %s (%s)\n", path, strerror(errno));
    close(fd);
    return n == (ssize_t)strlen(msg);
}

static bool osd_spawn(void)
{
    const char *env = getenv("TAIJI_OSD");
    char *path = env && *env ? xstrdup(env)
                             : xstrdup("/usr/libexec/taiji/taiji-osd");
    if (access(path, X_OK) != 0) {
        free(path);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        pid_t grand = fork();
        if (grand == 0) {
            execl(path, path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    free(path);
    return true;
}

static void volume_osd_send(int percent, bool muted)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "show %d %d", percent, muted ? 1 : 0);
    if (osd_send(msg)) return;
    if (!osd_spawn()) return;
    for (int i = 0; i < 6; i++) {
        usleep(80000);
        if (osd_send(msg)) return;
    }
}

static void volume_osd_hide(void)
{
    osd_send("hide");
}

static void brightness_osd_send(int percent)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "showb %d", percent);
    if (osd_send(msg)) return;
    if (!osd_spawn()) return;
    for (int i = 0; i < 6; i++) {
        usleep(80000);
        if (osd_send(msg)) return;
    }
}

void app_brightness_set(App *app, int percent)
{
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    if (!system_brightness_set(percent)) {
        ui_toast(app, "无法调节屏幕亮度");
        return;
    }
    /* The panel usually has only a handful of brightness levels.  Report the
     * value that was actually written, otherwise the OSD and the battery card
     * show different percentages (for example target 50% -> hardware 57%). */
    int actual = system_brightness_get();
    if (actual >= 0) percent = actual;
    brightness_osd_send(percent);
    if (app->modal == MODAL_CONTROL &&
        app->control_view == STATUS_BATTERY)
        ui_update_brightness_modal(app);
}

void app_brightness_step(App *app, int delta)
{
    int current = system_brightness_get();
    if (current < 0) {
        ui_toast(app, "无法调节屏幕亮度");
        return;
    }
    app_brightness_set(app, current + delta);
}

/* ── Managed task files ─────────────────────────────────────────────────── */

typedef struct {
    char id[80];
    char name[160];
    char state[24];
    pid_t pid;
    pid_t pgid;
    long started;
} ManagedTask;

static bool task_pid_alive(pid_t pid)
{
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

static char *tasks_dir_path(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return NULL;
    char *dir = path_join(rt, "taiji");
    char *tasks = path_join(dir, "tasks");
    free(dir);
    return tasks;
}

static bool task_read_file(const char *path, ManagedTask *t)
{
    memset(t, 0, sizeof(*t));
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return false;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "id") == 0) snprintf(t->id, sizeof(t->id), "%s", val);
        else if (strcmp(key, "name") == 0) snprintf(t->name, sizeof(t->name), "%s", val);
        else if (strcmp(key, "state") == 0) snprintf(t->state, sizeof(t->state), "%s", val);
        else if (strcmp(key, "pid") == 0) t->pid = (pid_t)atol(val);
        else if (strcmp(key, "pgid") == 0) t->pgid = (pid_t)atol(val);
        else if (strcmp(key, "started") == 0) t->started = atol(val);
    }
    free(data);
    return t->id[0] != '\0';
}

static int app_tasks_scan(App *app, char *oldest_id, size_t oldest_size)
{
    (void)app;
    if (oldest_id && oldest_size) oldest_id[0] = '\0';
    char *dir = tasks_dir_path();
    if (!dir) return 0;
    int count = 0;
    char **items = list_dir(dir, &count);
    int live = 0;
    long best = 0;
    bool have_best = false;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        if (n < 5 || strcmp(items[i] + n - 5, ".task") != 0) continue;
        char *path = path_join(dir, items[i]);
        ManagedTask t;
        if (task_read_file(path, &t) && task_pid_alive(t.pid)) {
            live++;
            if (!have_best || t.started < best) {
                best = t.started;
                have_best = true;
                if (oldest_id && oldest_size)
                    snprintf(oldest_id, oldest_size, "%s", t.id);
            }
        } else {
            unlink(path);
        }
        free(path);
    }
    free_strv(items, count);
    free(dir);
    return live;
}

bool app_task_state(App *app, const char *id, char *out, size_t out_size)
{
    if (!id || !*id || !out || out_size == 0) return false;
    (void)app;
    char *dir = tasks_dir_path();
    if (!dir) return false;
    int count = 0;
    char **items = list_dir(dir, &count);
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        size_t n = strlen(items[i]);
        if (n < 5 || strcmp(items[i] + n - 5, ".task") != 0) continue;
        char *path = path_join(dir, items[i]);
        ManagedTask t;
        if (task_read_file(path, &t) && strcmp(t.id, id) == 0) {
            if (!task_pid_alive(t.pid)) {
                unlink(path);
            } else {
                const char *label = "暂停";
                if (strcmp(t.state, "foreground") == 0) label = "前台";
                else if (strcmp(t.state, "background") == 0) label = "后台";
                else if (strcmp(t.state, "closed") == 0) label = "关闭";
                snprintf(out, out_size, "%s", label);
                found = true;
            }
        }
        free(path);
    }
    free_strv(items, count);
    free(dir);
    return found;
}

/* The UI scale setting also drives third-party app scaling.  We intentionally
 * do not change the compositor output scale: that would scale Taiji's own
 * window on top of the internal LVGL scale and double the effect. */
static void app_apply_system_scale(App *app)
{
    int pct = app_effective_ui_scale(app);
    double factor = (double)pct / 100.0;
    int gdk = (pct + 50) / 100;
    if (gdk < 1) gdk = 1;
    double gdk_dpi = factor / (double)gdk;
    const char *mode = getenv("TAIJI_SESSION_MODE");
    bool compositor = mode && strcmp(mode, "wayland") == 0;

    /* Wayland: the compositor scales third-party apps when they are in the
     * foreground. Keep toolkit variables neutral so an app is not scaled
     * twice. X11 has no compositor scaling, so toolkit variables remain the
     * fallback. */
    double env_factor = compositor ? 1.0 : factor;
    int env_gdk = compositor ? 1 : gdk;
    double env_dpi = compositor ? 1.0 : gdk_dpi;
    int cursor = compositor ? 24 : 24 * gdk;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", env_factor);
    setenv("QT_SCALE_FACTOR", buf, 1);
    setenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0", 1);
    snprintf(buf, sizeof(buf), "%d", env_gdk);
    setenv("GDK_SCALE", buf, 1);
    snprintf(buf, sizeof(buf), "%.2f", env_dpi);
    setenv("GDK_DPI_SCALE", buf, 1);
    snprintf(buf, sizeof(buf), "%d", cursor);
    setenv("XCURSOR_SIZE", buf, 1);

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) {
        char *dir = path_join(rt, "taiji");
        mkdir_p(dir);

        char *scale_path = path_join(dir, "scale");
        char *scale_data = xasprintf(
            "export QT_SCALE_FACTOR=%.2f\n"
            "export QT_AUTO_SCREEN_SCALE_FACTOR=0\n"
            "export GDK_SCALE=%d\n"
            "export GDK_DPI_SCALE=%.2f\n"
            "export XCURSOR_SIZE=%d\n"
            "export TAIJI_APP_SCALE=%.2f\n",
            env_factor, env_gdk, env_dpi, cursor, factor);
        write_file_atomic(scale_path, scale_data, strlen(scale_data));
        chmod(scale_path, 0600);
        free(scale_data);
        free(scale_path);

        /* Foreground-app scale used by the launch/resume helpers. */
        char *app_path = path_join(dir, "app_scale");
        char *app_data = xasprintf("%.2f\n", factor);
        write_file_atomic(app_path, app_data, strlen(app_data));
        chmod(app_path, 0600);
        free(app_data);
        free(app_path);

        /* Toolkit fallback if compositor scaling is unavailable. */
        char *fb_path = path_join(dir, "toolkit_scale");
        char *fb_data = xasprintf(
            "export QT_SCALE_FACTOR=%.2f\n"
            "export QT_AUTO_SCREEN_SCALE_FACTOR=0\n"
            "export GDK_SCALE=%d\n"
            "export GDK_DPI_SCALE=%.2f\n"
            "export XCURSOR_SIZE=%d\n",
            factor, gdk, gdk_dpi, 24 * gdk);
        write_file_atomic(fb_path, fb_data, strlen(fb_data));
        chmod(fb_path, 0600);
        free(fb_data);
        free(fb_path);
        free(dir);
    }

    if (mode && strcmp(mode, "x11") == 0) {
        int dpi = (int)(96.0 * factor + 0.5);
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "xrandr --dpi %d 2>/dev/null; printf 'Xft.dpi: %d\n' | xrdb -merge 2>/dev/null",
                 dpi, dpi);
        const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
        spawn_detached(argv);
    }
}

void app_open_overview(App *app)
{
    volume_osd_hide();
    if (app->ui_suspended) app_set_ui_suspended(app, false);
    if (app->modal != MODAL_NONE) {
        app->modal = MODAL_NONE;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
    }
    if (app->screen != SCREEN_OVERVIEW)
        app->overview_return_screen = app->screen;
    tasks_refresh(&app->tasks);
    apps_runtime_read(&app->runtime);
    int want = -1;
    if (strcmp(app->runtime.foreground, "app") == 0) {
        /* Match the running app in as many ways as the helper may expose it:
         * app id, process id, the id remembered at launch, or the launcher
         * pid. External windows are not TASK_KIND_APP, so do not filter kind. */
        if (app->runtime.app_id[0]) {
            for (int i = 0; i < app->tasks.count; i++) {
                if (strcmp(app->tasks.items[i].id, app->runtime.app_id) == 0) {
                    want = i;
                    break;
                }
            }
        }
        if (want < 0 && app->runtime.pid > 0) {
            for (int i = 0; i < app->tasks.count; i++) {
                if (app->tasks.items[i].pid == (int)app->runtime.pid) {
                    want = i;
                    break;
                }
            }
        }
        if (want < 0 && app->active_app_id[0]) {
            for (int i = 0; i < app->tasks.count; i++) {
                if (strcmp(app->tasks.items[i].id, app->active_app_id) == 0) {
                    want = i;
                    break;
                }
            }
        }
        if (want < 0 && app->launcher.pid > 0) {
            for (int i = 0; i < app->tasks.count; i++) {
                if (app->tasks.items[i].pid == (int)app->launcher.pid) {
                    want = i;
                    break;
                }
            }
        }
    } else if (strcmp(app->runtime.foreground, "music") == 0) {
        for (int i = 0; i < app->tasks.count; i++) {
            if (app->tasks.items[i].kind == TASK_KIND_MUSIC) {
                want = i;
                break;
            }
        }
    }
    if (want < 0) {
        for (int i = 0; i < app->tasks.count; i++) {
            if (app->tasks.items[i].focused) { want = i; break; }
        }
    }
    if (want < 0) {
        for (int i = 0; i < app->tasks.count; i++) {
            if (app->tasks.items[i].kind == TASK_KIND_TAIJI) { want = i; break; }
        }
    }
    app->overview_focus = want >= 0 ? want : 0;
    app->screen = SCREEN_OVERVIEW;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    app->overview_animate = true;
    app->overview_kill_armed = false;
    app_rebuild(app);
}

void app_close_overview(App *app)
{
    if (app->screen != SCREEN_OVERVIEW) return;
    app->screen = app->overview_return_screen;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    app->overview_kill_armed = false;
    app_rebuild(app);
}

void app_activate_task(App *app, int index)
{
    if (index < 0 || index >= app->tasks.count) return;
    TaskEntry *t = &app->tasks.items[index];
    if (t->kind == TASK_KIND_TAIJI) {
        app_close_overview(app);
        return;
    }
    if (t->kind == TASK_KIND_APP && t->id[0]) {
        const char *env = getenv("TAIJI_RESUME_HELPER");
        char *path = env && *env ? xstrdup(env)
                                 : xstrdup("/usr/libexec/taiji/taiji-resume");
        if (access(path, X_OK) != 0) {
            free(path);
            ui_toast(app, "恢复脚本未安装");
            return;
        }
        const char *argv[] = {path, t->id, NULL};
        spawn_detached(argv);
        free(path);
        app->screen = app->overview_return_screen;
        ui_toast(app, "正在切换任务");
        app_set_ui_suspended(app, true);
        return;
    }
    if (t->kind == TASK_KIND_EXTERNAL && t->con_id > 0) {
        const char *sock = getenv("SWAYSOCK");
        const char *mode = getenv("TAIJI_SESSION_MODE");
        if ((!sock || !*sock) && (!mode || !*mode)) {
            ui_toast(app, "切换外部窗口需要 sway 会话");
            return;
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "swaymsg '[con_id=%ld]' focus", t->con_id);
        const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
        spawn_detached(argv);
        app->screen = app->overview_return_screen;
        ui_toast(app, "正在切换任务");
        app_set_ui_suspended(app, true);
        return;
    }
    /* A managed task without a window can still be resumed by id. */
    if (t->id[0]) {
        const char *env = getenv("TAIJI_RESUME_HELPER");
        char *path = env && *env ? xstrdup(env)
                                 : xstrdup("/usr/libexec/taiji/taiji-resume");
        if (access(path, X_OK) == 0) {
            const char *argv[] = {path, t->id, NULL};
            spawn_detached(argv);
        }
        free(path);
        app->screen = app->overview_return_screen;
        app_set_ui_suspended(app, true);
    }
}

void app_close_task(App *app, int index)
{
    if (index < 0 || index >= app->tasks.count) return;
    TaskEntry *t = &app->tasks.items[index];
    if (t->kind == TASK_KIND_TAIJI) {
        app->overview_kill_armed = false;
        ui_toast(app, "不能关闭太极");
        return;
    }

    char title[192];
    snprintf(title, sizeof(title), "%s", t->title);

    if (t->kind == TASK_KIND_APP || t->kind == TASK_KIND_MUSIC) {
        const char *env = getenv("TAIJI_CLOSE_HELPER");
        char *path = env && *env ? xstrdup(env)
                                 : xstrdup("/usr/libexec/taiji/taiji-close-task");
        bool used = false;
        if (t->id[0] && access(path, X_OK) == 0) {
            const char *argv[] = {path, t->id, NULL};
            spawn_detached(argv);
            used = true;
        }
        free(path);
        if (!used) {
            AppRuntime rt;
            apps_runtime_read(&rt);
            bool managed = t->id[0] && strcmp(rt.app_id, t->id) == 0;
            long pid = managed ? (long)rt.pid : (long)t->pid;
            long pgid = managed ? (long)rt.pgid : 0;
            char cmd[256];
            if (pgid > 0) {
                snprintf(cmd, sizeof(cmd),
                         "kill -TERM -%ld 2>/dev/null; sleep 1; kill -KILL -%ld 2>/dev/null",
                         pgid, pgid);
            } else if (pid > 0) {
                snprintf(cmd, sizeof(cmd),
                         "kill -TERM %ld 2>/dev/null; sleep 1; kill -KILL %ld 2>/dev/null",
                         pid, pid);
            } else {
                snprintf(cmd, sizeof(cmd), "true");
            }
            const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
            spawn_detached(argv);
            if (managed) {
                snprintf(rt.foreground, sizeof(rt.foreground), "taiji");
                snprintf(rt.app_state, sizeof(rt.app_state), "closed");
                apps_runtime_write(&rt);
            }
        }
    } else if (t->kind == TASK_KIND_EXTERNAL && t->con_id > 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "swaymsg '[con_id=%ld]' kill 2>/dev/null", t->con_id);
        const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
        spawn_detached(argv);
    }

    if (index + 1 < app->tasks.count) {
        memmove(&app->tasks.items[index], &app->tasks.items[index + 1],
                sizeof(TaskEntry) * (size_t)(app->tasks.count - index - 1));
    }
    app->tasks.count--;
    if (app->overview_focus >= app->tasks.count)
        app->overview_focus = app->tasks.count > 0 ? app->tasks.count - 1 : 0;
    if (app->overview_focus < 0) app->overview_focus = 0;
    app->overview_kill_armed = false;
    app_rebuild(app);
    char msg[256];
    snprintf(msg, sizeof(msg), "已关闭 %s", title);
    ui_toast(app, msg);
}

void app_open_control_center(App *app)
{
    app_open_control_center_view(app, STATUS_WIFI);
}

void app_control_toggle_wifi(App *app)
{
    char error[256] = {0};
    bool enable = !app->wifi.enabled;
    if (network_wifi_toggle(enable, error, sizeof(error))) {
        network_wifi_scan_async(false);
        system_status_poll(&app->sys);
        ui_update_status(app);
        ui_toast(app, enable ? "Wi-Fi 已开启" : "Wi-Fi 已关闭");
    } else {
        ui_toast(app, error[0] ? error : "无法切换 Wi-Fi");
    }
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
}

void app_control_toggle_bluetooth(App *app)
{
    char error[256] = {0};
    bool enable = !app->bt.enabled;
    if (bluetooth_toggle(enable, error, sizeof(error))) {
        app->bt.enabled = enable;
        bluetooth_scan_async(false);
        ui_toast(app, enable ? "蓝牙已开启" : "蓝牙已关闭");
    } else {
        ui_toast(app, error[0] ? error : "无法切换蓝牙");
    }
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
}

void app_control_rescan_wifi(App *app)
{
    if (app->control_radio == 1) {
        snprintf(app->bt.message, sizeof(app->bt.message), "正在扫描蓝牙...");
        app->modal = MODAL_CONTROL;
        ui_refresh_control_modal(app);
        bluetooth_scan_async(true);
        ui_toast(app, "正在扫描蓝牙...");
    } else {
        network_wifi_scan_async(true);
        app->modal = MODAL_CONTROL;
        ui_refresh_control_modal(app);
        ui_toast(app, "正在扫描 Wi-Fi...");
    }
}

void app_control_select_radio(App *app, int radio)
{
    if (radio != 0 && radio != 1) return;
    if (app->control_radio == radio) {
        app->control_expanded = !app->control_expanded;
    } else {
        app->control_radio = radio;
        app->control_expanded = true;
    }
    app->control_col = 0;
    app->control_focus = 0;
    if (radio == 1) {
        /* 点进蓝牙页时立即开始 discovery，而不是只读 BlueZ 缓存。 */
        snprintf(app->bt.message, sizeof(app->bt.message), "正在扫描蓝牙...");
        bluetooth_scan_async(true);
        ui_toast(app, "正在扫描蓝牙...");
    }
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
}

void app_control_connect(App *app, int index)
{
    if (index < 0 || index >= app->wifi.count) return;
    WifiNetwork *n = &app->wifi.items[index];
    if (n->active) {
        ui_toast(app, "这个 Wi-Fi 已经连接");
        return;
    }
    snprintf(app->wifi_connect_ssid, sizeof(app->wifi_connect_ssid), "%s", n->ssid);
    if (n->secure) {
        app_open_text(app, TEXT_WIFI_PASSWORD, 0, "Wi-Fi 密码",
                      "", "输入密码；留空则尝试使用系统已保存的密码");
    } else {
        app_do_wifi_connect(app, n->ssid, "");
    }
}

void app_control_connect_bluetooth(App *app, int index)
{
    if (index < 0 || index >= app->bt.count) return;
    BtDevice *d = &app->bt.items[index];
    if (d->connected) {
        ui_toast(app, "这个蓝牙设备已经连接");
        return;
    }
    bluetooth_connect_async(d->address);
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
    char msg[192];
    snprintf(msg, sizeof(msg), "正在连接蓝牙设备：%s",
             d->name[0] ? d->name : d->address);
    ui_toast(app, msg);
}

void app_control_activate(App *app)
{
    if (app->control_col == 0) {
        app->control_expanded = !app->control_expanded;
        app->control_focus = 0;
        app->modal = MODAL_CONTROL;
        ui_refresh_control_modal(app);
    } else if (app->control_radio == 1) {
        app_control_connect_bluetooth(app, app->control_focus);
    } else {
        app_control_connect(app, app->control_focus);
    }
}

void app_control_volume_set(App *app, int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (!app->sysvol.available) system_volume_poll(&app->sysvol);
    if (!app->sysvol.available) {
        ui_toast(app, "系统音量不可用（需要 PipeWire / wpctl）");
        return;
    }
    if (!system_volume_apply(&app->sysvol, volume)) {
        ui_toast(app, "无法设置系统音量");
        return;
    }
    if (app->ui.modal && app->control_view == STATUS_VOLUME)
        ui_update_volume_modal(app);
    if (app->control_osd) app->control_osd_until = lv_tick_get() + 2000;
    ui_update_status(app);
}

void app_control_volume(App *app, int delta)
{
    if (!app->sysvol.available) system_volume_poll(&app->sysvol);
    if (!app->sysvol.available) {
        ui_toast(app, "系统音量不可用（需要 PipeWire / wpctl）");
        return;
    }
    if (!system_volume_delta(&app->sysvol, delta)) {
        ui_toast(app, "无法设置系统音量");
        return;
    }
    if (app->ui.modal && app->control_view == STATUS_VOLUME)
        ui_update_volume_modal(app);
    ui_update_status(app);
}

int app_ui_scale_max(App *app)
{
    (void)app;
    /* All scale steps up to 3x are selectable on every panel. */
    return 300;
}

int app_effective_ui_scale(App *app)
{
    int pct = app->config.ui_scale;
    if (app->config.ui_scale_auto) {
        int h = app->backend.height > 0 ? app->backend.height : 720;
        if (app->backend.fullscreen) {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(0, &r) == 0 && r.h > 0) h = r.h;
        }
        int raw = h * 100 / 720;
        int best = k_ui_scale_steps[0];
        int best_d = abs(raw - best);
        for (int i = 1; i < UI_SCALE_STEP_COUNT; i++) {
            int d = abs(raw - k_ui_scale_steps[i]);
            if (d < best_d) { best_d = d; best = k_ui_scale_steps[i]; }
        }
        pct = best;
    }
    if (pct < 100) pct = 100;
    if (pct > 300) pct = 300;
    int max = app_ui_scale_max(app);
    if (pct > max) pct = max;
    return pct;
}

void app_control_set_perf(App *app, int mode)
{
    if (!app->perf.available) {
        ui_toast(app, "当前系统不支持切换性能模式");
        return;
    }
    char error[256] = {0};
    bool ok = perf_set(&app->perf, mode, error, sizeof(error));
    if (ok) {
        app->power_module = 0;
        int idx = 0;
        for (int i = 0; i < 3; i++) {
            if (!app->perf.modes[i]) continue;
            if (i == mode) { app->control_focus = idx; break; }
            idx++;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "性能模式：%s", perf_mode_label(mode));
        ui_toast(app, msg);
        ui_update_status(app);
    } else {
        ui_toast(app, error[0] ? error : "无法切换性能模式");
    }
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
}

void app_control_set_gpu(App *app, int mode)
{
    if (!app->gpu.available) {
        ui_toast(app, "当前系统不支持切换 GPU 性能模式");
        return;
    }
    char error[256] = {0};
    bool ok = gpu_set(&app->gpu, mode, error, sizeof(error));
    if (ok) {
        app->power_module = 1;
        app->control_focus = mode_index_of(app->gpu.modes, mode);
        char msg[96];
        snprintf(msg, sizeof(msg), "GPU 模式：%s", gpu_mode_label(mode));
        ui_toast(app, msg);
    } else {
        ui_toast(app, error[0] ? error : "无法切换 GPU 模式");
    }
    app->modal = MODAL_CONTROL;
    ui_refresh_control_modal(app);
}

/* ── Modals: text / confirm / choice ─────────────────────────────────────── */

void app_open_text(App *app, TextPurpose purpose, int arg, const char *title,
                   const char *value, const char *hint)
{
    app->modal = MODAL_TEXT;
    app->text_purpose = purpose;
    app->modal_arg = arg;
    snprintf(app->modal_title, sizeof(app->modal_title), "%s", title ? title : "");
    snprintf(app->modal_hint, sizeof(app->modal_hint), "%s", hint ? hint : "");
    snprintf(app->input_buf, sizeof(app->input_buf), "%s", value ? value : "");
    app->input_cursor = (int)strlen(app->input_buf);
    ui_set_input_active(true);
    ui_build_modal(app);
}

void app_open_confirm(App *app, ConfirmPurpose purpose, int arg,
                      const char *title, const char *body)
{
    app->modal = MODAL_CONFIRM;
    app->confirm_purpose = purpose;
    if (purpose != CONFIRM_PAUSE_LIMIT) {
        app->pending_app_id[0] = '\0';
        app->pending_evict_id[0] = '\0';
    }
    app->modal_arg = arg;
    app->confirm_focus = 0;
    snprintf(app->modal_title, sizeof(app->modal_title), "%s", title ? title : "");
    snprintf(app->modal_body, sizeof(app->modal_body), "%s", body ? body : "");
    ui_build_modal(app);
}

void app_open_choice(App *app, ChoicePurpose purpose, const char *title)
{
    app->modal = MODAL_CHOICE;
    app->choice_purpose = purpose;
    app->choice_focus = 0;
    if (purpose == CHOICE_THEME)
        app->choice_focus = app->config.theme_auto ? theme_preset_count()
                                                   : app->config.theme;
    else if (purpose == CHOICE_COLUMNS) {
        int vals[] = {0, 4, 5, 6, 7, 8};
        for (int i = 0; i < 6; i++) if (vals[i] == app->grid_columns) app->choice_focus = i;
    } else if (purpose == CHOICE_SORT) app->choice_focus = app->sort_mode;
    else if (purpose == CHOICE_FILTER) app->choice_focus = app->filter_mode;
    else if (purpose == CHOICE_LANGUAGE)
        app->choice_focus = app->config.language == 1 ? 1 : 0;
    else if (purpose == CHOICE_TIMEZONE) {
        app->choice_focus = 0;
    }
    else if (purpose == CHOICE_RENDERER) {
        if (strstr(app->renderer_name, "gles") || strstr(app->renderer_name, "GLES"))
            app->choice_focus = 1;
        else if (strstr(app->renderer_name, "vulkan") ||
                 strstr(app->renderer_name, "Vulkan"))
            app->choice_focus = 2;
        else app->choice_focus = 0;
    }
    else if (purpose == CHOICE_UI_SCALE) {
        int vals[16];
        int max = app_ui_scale_max(app);
        int n = app_ui_scale_values(vals, 16, max);
        for (int i = 0; i < n; i++)
            if (vals[i] == app->config.ui_scale) app->choice_focus = i;
        if (app->config.ui_scale > max) app->choice_focus = n > 0 ? n - 1 : 0;
    }
    snprintf(app->modal_title, sizeof(app->modal_title), "%s", title ? title : "");
    ui_build_modal(app);
}

void app_close_modal(App *app)
{
    if (app->modal == MODAL_NONE || app->modal_closing) return;
    external_osk_hide(app);
    app->menu_info = false;
    if (app->screen == SCREEN_APPS) {
        app->app_badge = false;
        ui_update_app_badge(app);
    }
    if (app->modal == MODAL_CONTROL && !app->control_closing) {
        app->control_closing = true;
        app->modal_closing = true;
        ui_set_input_active(false);
        ui_fade_out_modal(app);
        return;
    }
    app->core_pending_game_id[0] = '\0';
    app->core_picker_launch = false;
    app->modal_closing = true;
    ui_set_input_active(false);
    ui_fade_out_modal(app);
}

void app_do_choice(App *app, int index, int value)
{
    if (app->choice_purpose == CHOICE_CORE) {
        if (index < 0 || index >= app->core_item_count) return;
        CoreChoiceItem item = app->core_items[index];
        Platform *p = library_find_platform(&app->library, app->core_platform_id);
        if (item.kind == CORE_ITEM_ALL) {
            core_picker_show_all(app);
            return;
        }
        if (item.kind == CORE_ITEM_BACK) {
            core_picker_show_compatible(app);
            return;
        }
        if (item.kind == CORE_ITEM_MANUAL) {
            int pidx = -1;
            for (int i = 0; i < app->library.platform_count; i++) {
                if (strcmp(app->library.platforms[i].id, app->core_platform_id) == 0) {
                    pidx = i;
                    break;
                }
            }
            if (pidx < 0) {
                ui_toast(app, "找不到该平台");
                return;
            }
            const char *cmd = config_command_for(&app->config, app->core_platform_id);
            char buf[1024];
            if (cmd && *cmd) {
                snprintf(buf, sizeof(buf), "%s", cmd);
            } else {
#ifdef __APPLE__
                char *def = catalog_default_command(app->core_platform_id, true);
#else
                char *def = catalog_default_command(app->core_platform_id, false);
#endif
                snprintf(buf, sizeof(buf), "%s", def ? def : "");
                free(def);
            }
            app_open_text(app, TEXT_COMMAND, pidx, "编辑启动命令", buf,
                          "使用 %ROM% 表示游戏路径，%NAME% 表示名称，%DIR% 表示目录。");
            return;
        }
        if (item.kind == CORE_ITEM_AUTO || item.kind == CORE_ITEM_CORE) {
            if (!p) {
                app_close_modal(app);
                return;
            }
            char pending[80];
            snprintf(pending, sizeof(pending), "%s", app->core_pending_game_id);
            bool launch = app->core_picker_launch && pending[0];
            if (item.kind == CORE_ITEM_AUTO) {
                /* Empty entry means "decided: use the automatic core", so the
                 * first-launch picker does not appear again. */
                config_set_command(&app->config, p->id, "");
            } else {
                char *quoted = shell_quote(item.path);
                char *cmd = xasprintf("retroarch -L %s %%ROM%%", quoted);
                free(quoted);
                config_set_command(&app->config, p->id, cmd);
                free(cmd);
            }
            app_save_settings(app);
            app_close_modal(app);
            if (launch) {
                Game *g = library_find_game(&app->library, pending);
                if (g) {
                    app_launch_game_now(app, g, NULL);
                    return;
                }
            }
            app_rebuild(app);
            if (item.kind == CORE_ITEM_AUTO)
                ui_toast(app, "已使用自动推荐核心");
            else if (item.compatible)
                ui_toast(app, "已选择该核心");
            else
                ui_toast(app, "已选择未验证核心，可能无法运行此平台");
            return;
        }
        return;
    }
    (void)index;
    if (app->choice_purpose == CHOICE_THEME) {
        if (value >= theme_preset_count()) {
            app->config.theme_auto = true;
        } else {
            app->config.theme_auto = false;
            app->config.theme = value;
        }
        app_save_settings(app);
        app_close_modal(app);
        app_apply_theme(app, true);
        ui_toast(app, app->config.theme_auto ? "已切换为自动主题" : "主题已切换");
    } else if (app->choice_purpose == CHOICE_COLUMNS) {
        app->config.grid_columns = value;
        app->grid_columns = value;
        app_save_settings(app);
        app_close_modal(app);
        app_rebuild(app);
    } else if (app->choice_purpose == CHOICE_SORT) {
        app->sort_mode = value;
        app->game_focus = 0;
        app_close_modal(app);
        app_rebuild(app);
    } else if (app->choice_purpose == CHOICE_FILTER) {
        app->filter_mode = value;
        app->game_focus = 0;
        app_close_modal(app);
        app_rebuild(app);
    } else if (app->choice_purpose == CHOICE_UI_SCALE) {
        app->config.ui_scale_auto = false;
        app->config.ui_scale = value;
        app_save_settings(app);
        app_apply_system_scale(app);
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "界面缩放已更新");
    } else if (app->choice_purpose == CHOICE_RENDERER) {
        if (value == 1) snprintf(app->config.renderer, sizeof(app->config.renderer), "opengles2");
        else if (value == 2) snprintf(app->config.renderer, sizeof(app->config.renderer), "vulkan");
        else snprintf(app->config.renderer, sizeof(app->config.renderer), "opengl");
        app_save_settings(app);
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "渲染器已保存，重启太极后生效");
    } else if (app->choice_purpose == CHOICE_LANGUAGE) {
        app_close_modal(app);
        app_set_language(app, value == 1 ? 1 : 0);
    } else if (app->choice_purpose == CHOICE_EXTRACT_TARGET) {
        app_close_modal(app);
        if (value == 0) app_files_extract_here(app);
        else if (value == 1) app_files_pick_extract_dir(app);
    } else if (app->choice_purpose == CHOICE_OPEN_WITH) {
        bool builtin = app->files_open_with_builtin;
        if (builtin && value == 0) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s", app->files_open_with_path);
            app_close_modal(app);
            editor_open(app, path);
            return;
        }
        int idx = builtin ? value - 1 : value;
        int app_index = (idx >= 0 && idx < app->files_open_with_count) ?
                        app->files_open_with_apps[idx] : -1;
        app_close_modal(app);
        if (app_index >= 0) app_files_open_with(app, app_index, false);
    } else if (app->choice_purpose == CHOICE_ARCHIVE_FORMAT) {
        app_close_modal(app);
        app_files_compress_now(app, value);
    } else if (app->choice_purpose == CHOICE_TIMEZONE) {
        const char *tz = app_timezone_option_id(app, value);
        char tzcopy[128];
        snprintf(tzcopy, sizeof(tzcopy), "%s", tz);
        char error[256] = {0};
        app_close_modal(app);
        if (!system_timezone_set(tzcopy, error, sizeof(error))) {
            ui_toast(app, error[0] ? error : "时区设置失败");
        } else {
            app_rebuild(app);
            char msg[256];
            if (i18n_language() == 1)
                snprintf(msg, sizeof(msg), "Time zone set to %s", tzcopy);
            else
                snprintf(msg, sizeof(msg), "时区已设置为 %s", tzcopy);
            ui_toast(app, msg);
        }
    }
}

/* ── Settings data ────────────────────────────────────────────────────────── */

int app_settings_category_count(void) { return 7; }

int app_settings_rail_count(void) { return 6; }

int app_settings_rail_cat(int index)
{
    static const int order[] = { 5, 2, 0, 1, 3, 4 };
    if (index < 0 || index >= 6) return 5;
    return order[index];
}

int app_settings_rail_index(int cat)
{
    for (int i = 0; i < 6; i++)
        if (app_settings_rail_cat(i) == cat) return i;
    return 0;
}

const char *app_settings_category_name(int cat, const char **subtitle)
{
    static const char *names[] = {"游戏库", "模拟器", "显示", "手柄", "应用", "系统", "关于"};
    static const char *subs[] = {
        "游戏目录与扫描工具", "每个平台的启动命令", "主题、网格与界面效果",
        "确认键、互换与手柄测试", "应用启动、返回与后台策略",
        "时区与系统语言", "版本、组件与操作说明"
    };
    if (cat < 0 || cat > 6) cat = 0;
    if (subtitle) *subtitle = subs[cat];
    return names[cat];
}

int app_settings_page_size(int cat)
{
    if (cat == 0) return 8;
    if (cat == 1) return 7;
    if (cat == 2) return 14;
    if (cat == 3) return 6;
    if (cat == 4) return 8;
    return 0;
}

void app_settings_rows(App *app, int *count)
{
    if (app->settings_cat == 0) *count = 8;
    else if (app->settings_cat == 1) *count = app->library.platform_count + 1;
    else if (app->settings_cat == 2) *count = 14;
    else if (app->settings_cat == 3) *count = 6;
    else if (app->settings_cat == 4) *count = 9;
    else if (app->settings_cat == 5) *count = 8;
    else *count = 0;
}

static const char *settings_core_status(App *app, const char *platform_id,
                                        char *buf, size_t n)
{
    const char *cmd = config_command_for(&app->config, platform_id);
    LibretroCore cores[64];
    int nc = libretro_compatible(platform_id, cores, 64);

    if (!cmd) {
#ifdef __APPLE__
        char *def = catalog_default_command(platform_id, true);
        bool retro = false;
        bool none = false;
#else
        char *def = catalog_default_command(platform_id, false);
        bool retro = def && strncmp(def, "retroarch", 9) == 0;
        bool none = def && strncmp(def, "xdg-open", 8) == 0;
#endif
        if (retro || none) {
            if (nc == 0)
                snprintf(buf, n, "未找到兼容核心 / 可选其他核心");
            else if (nc == 1)
                snprintf(buf, n, "自动 / %s", cores[0].label);
            else
                snprintf(buf, n, "未选择 / 首次启动时选择");
        } else if (def && *def) {
            if (strlen(def) > 46)
                snprintf(buf, n, "默认 / %.40s...", def);
            else
                snprintf(buf, n, "默认 / %s", def);
        } else {
            snprintf(buf, n, "未配置");
        }
        free(def);
        return buf;
    }

    if (!*cmd) {
        if (nc > 0) snprintf(buf, n, "自动 / %s", cores[0].label);
        else snprintf(buf, n, "未找到兼容核心 / 可选其他核心");
        return buf;
    }

    char path[512];
    if (libretro_command_core(cmd, path, sizeof(path))) {
        const char *base = path_basename(path);
        size_t len = strlen(base);
        static const char suffix[] = "_libretro.so";
        size_t suffix_len = sizeof(suffix) - 1;
        if (len > suffix_len && strcmp(base + len - suffix_len, suffix) == 0)
            len -= suffix_len;
        char name[64];
        snprintf(name, sizeof(name), "%.*s", (int)len, base);
        char label[128];
        libretro_label_for_name(name, label, sizeof(label));
        if (!file_exists(path))
            snprintf(buf, n, "核心失效 / %s", label);
        else
            snprintf(buf, n, "核心 / %s", label);
        return buf;
    }

    if (strlen(cmd) > 46)
        snprintf(buf, n, "自定义 / %.38s...", cmd);
    else
        snprintf(buf, n, "自定义 / %s", cmd);
    return buf;
}

static const char *renderer_display_name(const char *raw)
{
    if (!raw || !*raw) return "自动";
    if (strstr(raw, "vulkan") || strstr(raw, "Vulkan")) return "VULKAN";
    if (strstr(raw, "gles") || strstr(raw, "GLES")) return "OpenGL ES";
    if (strstr(raw, "opengl") || strstr(raw, "OpenGL")) return "OpenGL";
    if (strstr(raw, "software") || strstr(raw, "Software")) return "软件";
    return raw;
}

bool app_settings_row_info(App *app, int row, char *title, size_t title_size,
                           char *value, size_t value_size, int *kind)
{
    *kind = 0;
    if (app->settings_cat == 0) {
        switch (row) {
            case 0: snprintf(title, title_size, "游戏总数"); snprintf(value, value_size, "%d", app->library.game_count); break;
            case 1: snprintf(title, title_size, "平台数量"); snprintf(value, value_size, "%d", app->library.platform_count); break;
            case 2: snprintf(title, title_size, "收藏游戏"); snprintf(value, value_size, "%d", library_favorites_count(&app->library)); break;
            case 3: {
                char *s = format_playtime(library_total_play_seconds(&app->library));
                snprintf(title, title_size, "累计游玩");
                snprintf(value, value_size, "%s", s);
                free(s);
                break;
            }
            case 4: snprintf(title, title_size, "添加游戏目录"); value[0] = '\0'; *kind = 1; break;
            case 5: snprintf(title, title_size, "管理游戏目录"); snprintf(value, value_size, "%d 个目录", app->config.root_count); *kind = 1; break;
            case 6: snprintf(title, title_size, "立即扫描游戏库"); value[0] = '\0'; *kind = 1; break;
            case 7: snprintf(title, title_size, "启动时自动扫描"); snprintf(value, value_size, "%s", app->config.scan_on_start ? "开启" : "关闭"); *kind = 2; break;
            default: return false;
        }
        return true;
    }
    if (app->settings_cat == 1) {
        if (row < app->library.platform_count) {
            Platform *p = &app->library.platforms[row];
            snprintf(title, title_size, "%s  /  %s", catalog_zh_name(p->id), p->short_name);
            char buf[512];
            settings_core_status(app, p->id, buf, sizeof(buf));
            snprintf(value, value_size, "%s", buf);
            *kind = 1;
        } else if (row == app->library.platform_count) {
            snprintf(title, title_size, "恢复所有默认命令");
            value[0] = '\0';
            *kind = 1;
        } else return false;
        return true;
    }
    if (app->settings_cat == 2) {
        if (row == 0) {
            int det = backend_display_refresh_hz(&app->backend);
            snprintf(title, title_size, "刷新率");
            if (app->config.refresh_hz == 0)
                snprintf(value, value_size, "自动 (%d Hz)", det);
            else
                snprintf(value, value_size, "%d Hz", app->config.refresh_hz);
            *kind = 3;
            return true;
        }
        if (row == 1) {
            int pct = app->config.render_scale;
            if (pct < 50 || pct > 100) pct = 100;
            snprintf(title, title_size, "渲染分辨率");
            snprintf(value, value_size, "%d%% (%dx%d)", pct,
                     app->backend.width, app->backend.height);
            *kind = 3;
            return true;
        }
        if (row == 2) {
            snprintf(title, title_size, "SDL 渲染器");
            snprintf(value, value_size, "%s",
                     renderer_display_name(app->renderer_name));
            *kind = 1;
            return true;
        }
        row -= 3;
        switch (row) {
            case 0: {
                int max = app_ui_scale_max(app);
                int eff = app_effective_ui_scale(app);
                if (app->config.ui_scale_auto)
                    snprintf(value, value_size, "自动 (%d%%)", eff);
                else if (app->config.ui_scale > max)
                    snprintf(value, value_size, "%d%% (本机上限 %d%%)",
                             app->config.ui_scale, max);
                else
                    snprintf(value, value_size, "%d%%", app->config.ui_scale);
                snprintf(title, title_size, "界面缩放");
                *kind = 3;
                break;
            }
            case 1:
                snprintf(title, title_size, "主题预设");
                snprintf(value, value_size, "%s",
                         app->config.theme_auto ? "自动（深色/浅色）" :
                         theme_preset_name(app->config.theme));
                *kind = 3;
                break;
            case 2: {
                char colbuf[32];
                if (app->grid_columns) snprintf(colbuf, sizeof(colbuf), "%d 列", app->grid_columns);
                else snprintf(colbuf, sizeof(colbuf), "自动");
                snprintf(title, title_size, "游戏网格列数");
                snprintf(value, value_size, "%s", colbuf);
                *kind = 3;
                break;
            }
            case 3: snprintf(title, title_size, "显示封面侧栏"); snprintf(value, value_size, "%s", app->config.show_cover_panel ? "开启" : "关闭"); *kind = 2; break;
            case 4: snprintf(title, title_size, "减少动效"); snprintf(value, value_size, "%s", app->config.reduce_motion ? "开启" : "关闭"); *kind = 2; break;
            case 5: snprintf(title, title_size, "界面音效"); snprintf(value, value_size, "%s", app->config.sound ? "开启" : "关闭"); *kind = 2; break;
            case 6: snprintf(title, title_size, "界面音效音量"); snprintf(value, value_size, "%d%%", app->config.volume); *kind = 3; break;
            case 7: snprintf(title, title_size, "全屏显示"); snprintf(value, value_size, "%s", app->backend.fullscreen ? "开启" : "关闭"); *kind = 2; break;
            case 8: snprintf(title, title_size, "低内存模式"); snprintf(value, value_size, "%s",
                             app->config.low_power ? "开启" : "关闭"); *kind = 2; break;
            case 9: {
                snprintf(title, title_size, "自动深色时间");
                if (app->config.auto_dark_min < 0) {
                    snprintf(value, value_size, "19:00（自动）");
                } else {
                    int m = app->config.auto_dark_min;
                    snprintf(value, value_size, "%02d:%02d", m / 60, m % 60);
                }
                *kind = 1;
                break;
            }
            case 10: {
                snprintf(title, title_size, "自动浅色时间");
                if (app->config.auto_light_min < 0) {
                    snprintf(value, value_size, "07:00（自动）");
                } else {
                    int m = app->config.auto_light_min;
                    snprintf(value, value_size, "%02d:%02d", m / 60, m % 60);
                }
                *kind = 1;
                break;
            }
            default: return false;
        }
        return true;
    }
    if (app->settings_cat == 4) {
        static const char *modes[] = {"自动", "太极留在后台", "太极退出"};
        const char *session = getenv("TAIJI_SESSION_MODE");
        switch (row) {
            case 0: snprintf(title, title_size, "默认启动方式");
                snprintf(value, value_size, "%s", modes[app->config.app_mode % 3]); *kind = 3; break;
            case 1: snprintf(title, title_size, "最大暂停应用");
                snprintf(value, value_size, "%d 个", app->config.max_paused_apps); *kind = 3; break;
            case 2: snprintf(title, title_size, "低内存自动接管");
                snprintf(value, value_size, "%s", app->config.low_power ? "开启" : "关闭"); *kind = 2; break;
            case 3: snprintf(title, title_size, "应用数量");
                snprintf(value, value_size, "%d 个", app->apps.count); break;
            case 4: snprintf(title, title_size, "当前会话");
                snprintf(value, value_size, "%s", session && *session ? session : "桌面"); break;
            case 5: {
                const char *display = getenv("DISPLAY");
                snprintf(title, title_size, "XWayland");
                snprintf(value, value_size, "%s",
                         display && *display ? "可用" : "不可用");
            } break;
            case 6: snprintf(title, title_size, "返回键");
                snprintf(value, value_size, "%s",
                         app->backend.controller ?
                         backend_guide_label(&app->backend) : "Guide / Home");
                break;
            case 7: snprintf(title, title_size, "管理应用");
                snprintf(value, value_size, "打开应用库"); *kind = 1; break;
            case 8: snprintf(title, title_size, "扫描桌面应用");
                snprintf(value, value_size, "导入 .desktop 与图标"); *kind = 1; break;
            default: return false;
        }
        return true;
    }
    if (app->settings_cat == 5) {
        switch (row) {
            case 0: {
                char tz[128] = "";
                system_timezone_get(tz, sizeof(tz));
                snprintf(title, title_size, "时区");
                snprintf(value, value_size, "%s", tz[0] ? tz : "UTC");
                *kind = 1;
                break;
            }
            case 1: {
                char now[64];
                time_t t = time(NULL);
                struct tm tm;
                localtime_r(&t, &tm);
                strftime(now, sizeof(now), "%Y-%m-%d  %H:%M", &tm);
                snprintf(title, title_size, "日期与时间");
                snprintf(value, value_size, "%s", now);
                *kind = 1;
                break;
            }
            case 2: {
                bool ntp = false;
                snprintf(title, title_size, "自动时间同步");
                if (system_ntp_get(&ntp))
                    snprintf(value, value_size, "%s", ntp ? "开启" : "关闭");
                else
                    snprintf(value, value_size, "不可用");
                *kind = 2;
                break;
            }
            case 3:
                snprintf(title, title_size, "语言");
                snprintf(value, value_size, "%s",
                         app->config.language == 1 ? "English" : "中文");
                *kind = 3;
                break;
            case 4:
                snprintf(title, title_size, "关于本机");
                snprintf(value, value_size, "系统与版本信息");
                *kind = 1;
                break;
            case 5:
                snprintf(title, title_size, "开源许可");
                snprintf(value, value_size, "GPL-3.0-or-later");
                *kind = 1;
                break;
            case 6:
                snprintf(title, title_size, "重新运行初始设置");
                snprintf(value, value_size, "重新设置语言、时区与显示");
                *kind = 1;
                break;
            case 7:
                snprintf(title, title_size, "还原太极设置");
                snprintf(value, value_size, "恢复默认设置并重走初始设置");
                *kind = 1;
                break;
            default: return false;
        }
        return true;
    }
    if (app->settings_cat == 3) {
        switch (row) {
            case 0: snprintf(title, title_size, "已连接手柄"); {
                const char *name = backend_controller_name(&app->backend);
                snprintf(value, value_size, "%s", name && *name ? name : "未连接");
            } break;
            case 1: snprintf(title, title_size, "A / B 互换"); snprintf(value, value_size, "%s", app->config.swap_ab ? "任天堂式" : "标准"); *kind = 2; break;
            case 2: snprintf(title, title_size, "X / Y 互换"); snprintf(value, value_size, "%s", app->config.swap_xy ? "已互换" : "标准"); *kind = 2; break;
            case 3: snprintf(title, title_size, "PS × / ○ 互换"); snprintf(value, value_size, "%s", app->config.swap_ps ? "已互换" : "标准"); *kind = 2; break;
            case 4: snprintf(title, title_size, "恢复默认按键"); value[0] = '\0'; *kind = 1; break;
            case 5: snprintf(title, title_size, "手柄按键检测"); value[0] = '\0'; *kind = 1; break;
            default: return false;
        }
        return true;
    }
    return false;
}

static void app_cycle_render_scale(App *app, int dir)
{
    static const int vals[] = {100, 75, 67, 50};
    int idx = 0;
    for (int i = 0; i < 4; i++)
        if (vals[i] == app->config.render_scale) idx = i;
    idx = (idx + dir + 4) % 4;
    app->config.render_scale = vals[idx];
    app_save_settings(app);
    backend_set_render_scale(&app->backend, app->config.render_scale);
    app_rebuild(app);
}

/* ── OOBE setters ─────────────────────────────────────────────────────────── */

void app_oobe_apply_language(App *app, int lang)
{
    app->config.language = lang == 1 ? 1 : 0;
    i18n_set_language(app->config.language);
}

void app_oobe_apply_theme(App *app, int preset, bool auto_mode)
{
    if (auto_mode) {
        app->config.theme_auto = true;
    } else {
        if (preset < 0 || preset >= theme_preset_count()) preset = 0;
        app->config.theme_auto = false;
        app->config.theme = preset;
    }
    app->active_theme = app_theme_preset(app);
    theme_set_preset(app->active_theme);
}

void app_oobe_cycle_ui_scale(App *app, int dir)
{
    int steps[16];
    int max = app_ui_scale_max(app);
    int sn = app_ui_scale_values(steps, 16, max);
    int avail[17];
    int n = 0;
    avail[n++] = 0;
    for (int i = sn - 1; i >= 0; i--) avail[n++] = steps[i];
    int idx = 0;
    if (!app->config.ui_scale_auto) {
        for (int i = 1; i < n; i++)
            if (avail[i] == app->config.ui_scale) { idx = i; break; }
    }
    idx = (idx + dir + n) % n;
    if (avail[idx] == 0) {
        app->config.ui_scale_auto = true;
    } else {
        app->config.ui_scale_auto = false;
        app->config.ui_scale = avail[idx];
    }
    app_apply_system_scale(app);
    app_rebuild(app);
}

void app_oobe_cycle_refresh(App *app, int dir)
{
    static const int vals[] = {0, 60, 120};
    int idx = 0;
    for (int i = 0; i < 3; i++)
        if (vals[i] == app->config.refresh_hz) idx = i;
    idx = (idx + dir + 3) % 3;
    app->config.refresh_hz = vals[idx];
    backend_set_refresh_limit(&app->backend, app->config.refresh_hz);
    app_rebuild(app);
}

void app_oobe_cycle_render(App *app, int dir)
{
    static const int vals[] = {100, 75, 67, 50};
    int idx = 0;
    for (int i = 0; i < 4; i++)
        if (vals[i] == app->config.render_scale) idx = i;
    idx = (idx + dir + 4) % 4;
    app->config.render_scale = vals[idx];
    backend_set_render_scale(&app->backend, app->config.render_scale);
    app_rebuild(app);
}

static int oobe_days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

void app_oobe_date_open(App *app)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    app->oobe_date[0] = tm.tm_year + 1900;
    app->oobe_date[1] = tm.tm_mon + 1;
    app->oobe_date[2] = tm.tm_mday;
    app->oobe_date[3] = tm.tm_hour;
    app->oobe_date[4] = tm.tm_min;
    app->oobe_date_field = 0;
    app->modal = MODAL_OOBE_DATE;
    ui_build_modal(app);
}

void app_oobe_date_set_field(App *app, int field)
{
    if (field < 0 || field > 4) return;
    app->oobe_date_field = field;
    if (app->ui.oobe_date_boxes[field])
        ui_oobe_date_focus_update(app);
    else
        ui_refresh_modal(app);
}

void app_oobe_date_apply(App *app)
{
    char error[256] = {0};
    if (!system_datetime_set(app->oobe_date[0], app->oobe_date[1],
                             app->oobe_date[2], app->oobe_date[3],
                             app->oobe_date[4], error, sizeof(error))) {
        ui_toast(app, error[0] ? error : "修改日期失败");
        return;
    }
    app_close_modal(app);
    ui_toast(app, tr("日期与时间已设置"));
}

static void oobe_date_clamp(App *app)
{
    if (app->oobe_date[0] < 1970) app->oobe_date[0] = 1970;
    if (app->oobe_date[0] > 2100) app->oobe_date[0] = 2100;
    if (app->oobe_date[1] < 1) app->oobe_date[1] = 12;
    if (app->oobe_date[1] > 12) app->oobe_date[1] = 1;
    int max_day = oobe_days_in_month(app->oobe_date[0], app->oobe_date[1]);
    if (app->oobe_date[2] < 1) app->oobe_date[2] = max_day;
    if (app->oobe_date[2] > max_day) app->oobe_date[2] = 1;
    if (app->oobe_date[3] < 0) app->oobe_date[3] = 23;
    if (app->oobe_date[3] > 23) app->oobe_date[3] = 0;
    if (app->oobe_date[4] < 0) app->oobe_date[4] = 59;
    if (app->oobe_date[4] > 59) app->oobe_date[4] = 0;
}

static void app_oobe_date_input(App *app, InputAction action)
{
    int field = app->oobe_date_field;
    if (field < 0) field = 0;
    if (field > 4) field = 4;

    if (action == IN_CANCEL || action == IN_B) {
        app_close_modal(app);
        return;
    }
    if (action == IN_LEFT) {
        app->oobe_date_field = field > 0 ? field - 1 : 4;
        ui_refresh_modal(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_RIGHT) {
        app->oobe_date_field = field < 4 ? field + 1 : 0;
        ui_refresh_modal(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_UP || action == IN_DOWN) {
        int delta = action == IN_UP ? 1 : -1;
        app->oobe_date[field] += delta;
        oobe_date_clamp(app);
        ui_refresh_modal(app);
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) app_oobe_date_apply(app);
}

void app_oobe_cycle_paused(App *app, int dir)
{
    app->config.max_paused_apps += dir;
    if (app->config.max_paused_apps < 1) app->config.max_paused_apps = 6;
    if (app->config.max_paused_apps > 6) app->config.max_paused_apps = 1;
    app_rebuild(app);
}

/* 还原太极设置：配置文件、游戏库缓存和运行期状态都回到出厂默认，
 * 然后立刻重走初始设置。用户自己的游戏文件不会被删除。 */
static void app_clear_icon_cache(void)
{
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/icons", taiji_config_dir());
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char *path = path_join(dir, ent->d_name);
            if (path) {
                unlink(path);
                free(path);
            }
        }
        closedir(d);
        rmdir(dir);
    }
}

void app_reset_settings(App *app)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/config.ini", taiji_config_dir());
    unlink(path);
    snprintf(path, sizeof(path), "%s/library.tsv", taiji_config_dir());
    unlink(path);
    snprintf(path, sizeof(path), "%s/apps.tsv", taiji_config_dir());
    unlink(path);
    snprintf(path, sizeof(path), "%s/shares.tsv", taiji_config_dir());
    unlink(path);
    snprintf(path, sizeof(path), "%s/.desktop-imported", taiji_config_dir());
    unlink(path);
    app_clear_icon_cache();

    /* 本地 SMB 共享也一起清掉，恢复到出厂状态。 */
    smb_local_refresh(app);
    int guard = 0;
    while (app->smb_ui.local_count > 0 && guard++ < 64) {
        /* smb_local_remove 会重新载入列表，先复制名字再删除。 */
        char name[128];
        snprintf(name, sizeof(name), "%s", app->smb_ui.local[0].name);
        char err[160] = "";
        if (!smb_local_remove(app, name, err, sizeof(err))) break;
    }
    smb_local_refresh(app);
    app->smb_ui.local_count = 0;
    app->smb_ui.focus = 0;
    app->share_count = 0;

    library_free(&app->library);
    library_init(&app->library);
    apps_free(&app->apps);
    apps_init(&app->apps);
    apps_load(&app->apps);
#ifdef __linux__
    desktop_import_auto(&app->apps);
#endif
    if (app->screen == SCREEN_EDITOR) editor_close(app);
    app_files_favorites_reset();

    config_free(&app->config);
    config_init(&app->config);
    app->config.theme_auto = true;

    app->active_theme = app_theme_preset(app);
    theme_set_preset(app->active_theme);
    app_apply_theme(app, false);
    i18n_set_language(app->config.language);
    system_language_export(app->config.language);
    app_apply_gamepad_config(app);
    audio_set_enabled(app->config.sound);
    audio_set_volume(app->config.volume);
    app_apply_system_scale(app);
    backend_set_refresh_limit(&app->backend, app->config.refresh_hz);
    backend_set_render_scale(&app->backend, app->config.render_scale);

    app->config_missing = true;
    app->settings_about = false;
    app->settings_license = false;
    app->settings_on_rail = false;
    app->settings_row = 0;
    app->settings_cat = 5;
    app->status_on_bar = false;
    app->nav_on_bar = false;
    app->grid_columns = app->config.grid_columns;
    app->sort_mode = SORT_NAME;
    app->filter_mode = FILTER_ALL;
    app->game_focus = 0;
    app->game_page = 0;
    app->home_focus = 0;
    app->app_focus = 0;
    app->app_page = 0;
    app->app_cat_focus = 0;
    app->app_on_categories = false;
    snprintf(app->app_cat, sizeof(app->app_cat), "all");
    snprintf(app->collection_id, sizeof(app->collection_id), "all");
    app->selected_game_id[0] = '\0';
    app->files_show_hidden = true;
    oobe_start(app, false);
}


static void app_cycle_refresh(App *app, int dir)
{
    static const int vals[] = {0, 60, 120};
    int idx = 0;
    for (int i = 0; i < 3; i++)
        if (vals[i] == app->config.refresh_hz) idx = i;
    idx = (idx + dir + 3) % 3;
    app->config.refresh_hz = vals[idx];
    app_save_settings(app);
    backend_set_refresh_limit(&app->backend, app->config.refresh_hz);
    app_rebuild(app);
}

void app_settings_activate(App *app, int row)
{
    int kind = 0;
    char t[256], v[512];
    if (!app_settings_row_info(app, row, t, sizeof(t), v, sizeof(v), &kind)) return;
    if (app->settings_cat == 0) {
        if (row == 4) app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "",
                                    "输入 ROM 根目录路径，例如 ~/ROMs 或 /Volumes/Games");
        else if (row == 5) {
            app->modal = MODAL_ROOTS;
            app->roots_focus = 0;
            ui_build_modal(app);
        } else if (row == 6) app_rescan(app);
        else if (row == 7) {
            app->config.scan_on_start = !app->config.scan_on_start;
            app_save_settings(app);
            app_rebuild(app);
        } else {
            char hint[64];
            ui_button_hint(app, hint, sizeof(hint),
                           "使用方向键浏览，{A} 执行操作");
            ui_toast(app, hint);
        }
        return;
    }
    if (app->settings_cat == 1) {
        if (row < app->library.platform_count) {
            Platform *p = &app->library.platforms[row];
#ifdef __APPLE__
            const char *cmd = config_command_for(&app->config, p->id);
            char buf[1024];
            if (cmd && *cmd) {
                snprintf(buf, sizeof(buf), "%s", cmd);
            } else {
                char *def = catalog_default_command(p->id, true);
                snprintf(buf, sizeof(buf), "%s", def ? def : "");
                free(def);
            }
            app_open_text(app, TEXT_COMMAND, row, "编辑启动命令", buf,
                          "使用 %ROM% 表示游戏路径，%NAME% 表示名称，%DIR% 表示目录。");
#else
            app_open_core_picker(app, p->id, false, NULL);
#endif
        } else {
            for (int i = 0; i < app->config.command_count; i++) {
                free(app->config.commands[i].platform_id);
                free(app->config.commands[i].command);
            }
            free(app->config.commands);
            app->config.commands = NULL;
            app->config.command_count = 0;
            app_save_settings(app);
            app_rebuild(app);
            ui_toast(app, "已恢复默认启动命令");
        }
        return;
    }
    if (app->settings_cat == 2) {
        if (row == 0) { app_cycle_refresh(app, 1); return; }
        if (row == 1) { app_cycle_render_scale(app, 1); return; }
        if (row == 2) {
            app_open_choice(app, CHOICE_RENDERER, "SDL 渲染器（重启后生效）");
            return;
        }
        row -= 3;
        if (row == 0) app_open_choice(app, CHOICE_UI_SCALE, "界面缩放");
        else if (row == 1) app_open_choice(app, CHOICE_THEME, "选择主题");
        else if (row == 2) app_open_choice(app, CHOICE_COLUMNS, "游戏网格列数");
        else if (row == 3) { app->config.show_cover_panel = !app->config.show_cover_panel; app_save_settings(app); app_rebuild(app); }
        else if (row == 4) { app->config.reduce_motion = !app->config.reduce_motion; app_save_settings(app); app_rebuild(app); }
        else if (row == 5) { app->config.sound = !app->config.sound; app_save_settings(app); app_rebuild(app); }
        else if (row == 6) { app->config.volume = (app->config.volume + 10) % 110; audio_set_volume(app->config.volume); app_save_settings(app); app_rebuild(app); }
        else if (row == 7) { backend_toggle_fullscreen(&app->backend); app_rebuild(app); }
        else if (row == 8) { app->config.low_power = !app->config.low_power; app_save_settings(app); app_rebuild(app); }
        else if (row == 9 || row == 10) {
            bool dark = row == 9;
            int minutes = dark ? app->config.auto_dark_min : app->config.auto_light_min;
            char cur[16] = "";
            if (minutes >= 0)
                snprintf(cur, sizeof(cur), "%02d:%02d", minutes / 60, minutes % 60);
            app_open_text(app, dark ? TEXT_AUTO_DARK : TEXT_AUTO_LIGHT, 0,
                          dark ? "自动深色时间" : "自动浅色时间", cur,
                          "输入 HH:MM，例如 20:00；留空恢复默认 19:00 / 07:00。");
        }
        return;
    }
    if (app->settings_cat == 5) {
        if (row == 0) {
            app_open_choice(app, CHOICE_TIMEZONE, "时区");
        } else if (row == 1) {
            app_oobe_date_open(app);
        } else if (row == 2) {
            bool ntp = false;
            char err[160] = "";
            if (!system_ntp_get(&ntp)) {
                ui_toast(app, "系统不支持时间同步");
            } else if (system_ntp_set(!ntp, err, sizeof(err))) {
                ui_toast(app, !ntp ? "自动时间同步已开启" : "自动时间同步已关闭");
                app_rebuild(app);
            } else {
                ui_toast(app, err[0] ? err : "修改自动时间同步失败");
            }
        } else if (row == 3) {
            app_open_choice(app, CHOICE_LANGUAGE, "选择语言");
        } else if (row == 4) {
            app->settings_about = true;
            app->settings_license = false;
            app->settings_on_rail = false;
            app->settings_row = 0;
            app_rebuild(app);
        } else if (row == 5) {
            app->settings_about = false;
            app->settings_license = true;
            app->settings_on_rail = false;
            app->settings_row = 3;
            app_rebuild(app);
        } else if (row == 6) {
            app_open_confirm(app, CONFIRM_RERUN_OOBE, 0, "重新运行初始设置？",
                             "会重新走一遍语言、时区、主题和目录设置，完成后保存。");
        } else if (row == 7) {
            app_open_confirm(app, CONFIRM_RESET_TAIJI, 0, "还原太极设置？",
                             "所有设置会恢复默认，应用列表、文件收藏、共享书签和本地共享都会清空，游戏文件不会被删除。");
        }
        return;
    }
    if (app->settings_cat == 4) {
        if (row == 0) app->config.app_mode = (app->config.app_mode + 1) % 3;
        else if (row == 1) app->config.max_paused_apps = app->config.max_paused_apps % 6 + 1;
        else if (row == 2) app->config.low_power = !app->config.low_power;
        else if (row == 7) { app_goto_screen(app, SCREEN_APPS); return; }
        else if (row == 8) {
            app_desktop_scan_start(app, true);
            ui_toast(app, "正在扫描桌面应用...");
            return;
        }
        app_save_settings(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 3) {
        if (row == 1) { app->config.swap_ab = !app->config.swap_ab; app_save_settings(app); app_rebuild(app); }
        else if (row == 2) { app->config.swap_xy = !app->config.swap_xy; app_save_settings(app); app_rebuild(app); }
        else if (row == 3) { app->config.swap_ps = !app->config.swap_ps; app_save_settings(app); app_rebuild(app); }
        else if (row == 4) {
            app->config.custom_mapping = false;
            app->config.map_confirm = app->config.map_cancel = -1;
            app->config.map_menu1 = app->config.map_menu2 = -1;
            app->config.swap_ab = app->config.swap_xy = false;
            app->config.swap_ps = false;
            app_save_settings(app);
            app_rebuild(app);
            ui_toast(app, "已恢复默认按键");
        } else if (row == 5) {
            app->modal = MODAL_GAMEPAD;
            ui_build_modal(app);
        }
        return;
    }
}

void app_settings_adjust(App *app, int row, int dir)
{
    int kind = 0;
    char t[256], v[512];
    if (!app_settings_row_info(app, row, t, sizeof(t), v, sizeof(v), &kind)) return;
    if (app->settings_cat == 2) {
        if (row == 0) { app_cycle_refresh(app, dir); return; }
        if (row == 1) { app_cycle_render_scale(app, dir); return; }
        row -= 3;
    }
    if (app->settings_cat == 5 && row == 3) {
        app_set_language(app, app->config.language == 1 ? 0 : 1);
        return;
    }
    if (kind == 2) {
        if (app->settings_cat == 2 && row == 3) app->config.show_cover_panel = !app->config.show_cover_panel;
        else if (app->settings_cat == 2 && row == 4) app->config.reduce_motion = !app->config.reduce_motion;
        else if (app->settings_cat == 2 && row == 5) app->config.sound = !app->config.sound;
        else if (app->settings_cat == 2 && row == 7) { backend_toggle_fullscreen(&app->backend); app_rebuild(app); return; }
        else if (app->settings_cat == 2 && row == 8) { app->config.low_power = !app->config.low_power; app_save_settings(app); app_rebuild(app); return; }
        else if (app->settings_cat == 3 && row == 1) app->config.swap_ab = !app->config.swap_ab;
        else if (app->settings_cat == 3 && row == 2) app->config.swap_xy = !app->config.swap_xy;
        else if (app->settings_cat == 3 && row == 3) app->config.swap_ps = !app->config.swap_ps;
        app_save_settings(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 2 && row == 6) {
        app->config.volume += dir * 5;
        if (app->config.volume < 0) app->config.volume = 0;
        if (app->config.volume > 100) app->config.volume = 100;
        audio_set_volume(app->config.volume);
        app_save_settings(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 2 && row == 0) {
        /* Auto first, then descending so Right from Auto increases (or keeps)
         * the effective size instead of jumping to the smallest value. */
        int steps[16];
        int max = app_ui_scale_max(app);
        int sn = app_ui_scale_values(steps, 16, max);
        int avail[17];
        int n = 0;
        avail[n++] = 0;
        for (int i = sn - 1; i >= 0; i--) avail[n++] = steps[i];
        int idx = 0;
        if (!app->config.ui_scale_auto) {
            for (int i = 1; i < n; i++) {
                if (avail[i] == app->config.ui_scale) { idx = i; break; }
            }
        }
        idx = (idx + dir + n) % n;
        if (avail[idx] == 0) {
            app->config.ui_scale_auto = true;
        } else {
            app->config.ui_scale_auto = false;
            app->config.ui_scale = avail[idx];
        }
        app_save_settings(app);
        app_apply_system_scale(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 2 && row == 2) {
        int vals[] = {0, 4, 5, 6, 7, 8};
        int idx = 0;
        for (int i = 0; i < 6; i++) if (vals[i] == app->grid_columns) idx = i;
        idx = (idx + dir + 6) % 6;
        app->grid_columns = vals[idx];
        app->config.grid_columns = vals[idx];
        app_save_settings(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 4 && (row == 0 || row == 1 || row == 2)) {
        if (row == 0) app->config.app_mode = (app->config.app_mode + dir + 3) % 3;
        else if (row == 1) {
            app->config.max_paused_apps += dir;
            if (app->config.max_paused_apps < 1) app->config.max_paused_apps = 6;
            if (app->config.max_paused_apps > 6) app->config.max_paused_apps = 1;
        } else app->config.low_power = !app->config.low_power;
        app_save_settings(app);
        app_rebuild(app);
        return;
    }
    if (app->settings_cat == 2 && row == 1) {
        int n = theme_preset_count() + 1;
        int idx = app->config.theme_auto ? theme_preset_count() : app->config.theme;
        idx = (idx + dir + n) % n;
        if (idx >= theme_preset_count()) {
            app->config.theme_auto = true;
        } else {
            app->config.theme_auto = false;
            app->config.theme = idx;
        }
        app_save_settings(app);
        app_apply_theme(app, true);
    }
}

/* ── Menu ─────────────────────────────────────────────────────────────────── */

void app_menu_labels(App *app, const char **labels, MenuAction *actions, int *count)
{
    int n = 0;
    if (app->screen == SCREEN_HOME) {
        labels[n] = "搜索游戏"; actions[n++] = MENU_SEARCH;
        labels[n] = "立即扫描游戏库"; actions[n++] = MENU_SCAN;
        labels[n] = "添加游戏目录"; actions[n++] = MENU_ADD_ROOT;
        labels[n] = "排序与筛选"; actions[n++] = MENU_SORT;
        labels[n] = "切换主题"; actions[n++] = MENU_THEME;
        labels[n] = "前端设置"; actions[n++] = MENU_SETTINGS;
        labels[n] = "文件管理器"; actions[n++] = MENU_FILES;
        labels[n] = "操作帮助"; actions[n++] = MENU_HELP;
        labels[n] = "退出应用"; actions[n++] = MENU_QUIT;
    } else if (app->screen == SCREEN_GAMES || app->screen == SCREEN_FAVORITES || app->screen == SCREEN_RECENT) {
        labels[n] = "搜索游戏"; actions[n++] = MENU_SEARCH;
        labels[n] = "立即扫描游戏库"; actions[n++] = MENU_SCAN;
        labels[n] = "排序"; actions[n++] = MENU_SORT;
        labels[n] = "筛选"; actions[n++] = MENU_FILTER;
        labels[n] = "切换主题"; actions[n++] = MENU_THEME;
        labels[n] = "文件管理器"; actions[n++] = MENU_FILES;
        labels[n] = "操作帮助"; actions[n++] = MENU_HELP;
        labels[n] = "返回游戏库"; actions[n++] = MENU_BACK;
        labels[n] = "退出应用"; actions[n++] = MENU_QUIT;
    } else if (app->screen == SCREEN_APPS) {
        labels[n] = "添加应用"; actions[n++] = MENU_ADD_APP;
        labels[n] = "扫描桌面应用"; actions[n++] = MENU_IMPORT_DESKTOP;
        labels[n] = "切换主题"; actions[n++] = MENU_THEME;
        labels[n] = "文件管理器"; actions[n++] = MENU_FILES;
        labels[n] = "操作帮助"; actions[n++] = MENU_HELP;
        labels[n] = "返回游戏库"; actions[n++] = MENU_BACK;
        labels[n] = "退出应用"; actions[n++] = MENU_QUIT;
    } else if (app->screen == SCREEN_FILES) {
        bool has_sel = !app->files_on_sidebar && app->files_focus >= 0 &&
                       app->files_focus < app->files_list.count;
        FsEntry *sel = has_sel ? &app->files_list.items[app->files_focus] : NULL;
        bool is_dir = sel && sel->is_dir;
        bool is_archive = sel && !sel->is_dir && files_is_archive(sel->name);
        char fav_path[PATH_MAX];
        bool has_fav_path = app_files_favorite_path(app, fav_path, sizeof(fav_path));
        bool is_fav = has_fav_path && app_files_favorite_contains(fav_path);
        bool fav_current = !app->files_on_sidebar && (!sel || !is_dir);

        labels[n] = is_fav ? (fav_current ? "取消收藏当前文件夹" : "从收藏移除")
                           : (fav_current ? "收藏当前文件夹" : "添加到收藏");
        actions[n++] = MENU_FILE_FAVORITE;
        if (sel && !is_dir) {
            labels[n] = "打开"; actions[n++] = MENU_FILE_OPEN;
            labels[n] = "打开方式..."; actions[n++] = MENU_FILE_OPEN_WITH;
            if (is_archive) {
                labels[n] = "解压到此处"; actions[n++] = MENU_FILE_EXTRACT;
            }
        }
        if (sel) {
            labels[n] = "压缩..."; actions[n++] = MENU_FILE_COMPRESS;
            labels[n] = "重命名"; actions[n++] = MENU_FILE_RENAME;
            labels[n] = "复制"; actions[n++] = MENU_FILE_COPY;
            labels[n] = "剪切"; actions[n++] = MENU_FILE_CUT;
        }
        labels[n] = "粘贴"; actions[n++] = MENU_FILE_PASTE;
        if (sel) {
            labels[n] = "移到回收站"; actions[n++] = MENU_FILE_TRASH;
            labels[n] = "永久删除"; actions[n++] = MENU_FILE_DELETE;
        }
        labels[n] = "新建文件夹"; actions[n++] = MENU_FILE_NEW;
        labels[n] = "新建文本文件"; actions[n++] = MENU_FILE_NEW_TEXT;
        labels[n] = "共享…"; actions[n++] = MENU_FILE_SHARE;
        if (sel) {
            labels[n] = "属性"; actions[n++] = MENU_FILE_PROPERTIES;
        }
    } else {
        labels[n] = "立即扫描游戏库"; actions[n++] = MENU_SCAN;
        labels[n] = "添加游戏目录"; actions[n++] = MENU_ADD_ROOT;
        labels[n] = "切换主题"; actions[n++] = MENU_THEME;
        labels[n] = "文件管理器"; actions[n++] = MENU_FILES;
        labels[n] = "操作帮助"; actions[n++] = MENU_HELP;
        labels[n] = "退出应用"; actions[n++] = MENU_QUIT;
    }
    *count = n;
}

void app_run_menu_action(App *app, MenuAction action)
{
    app_close_modal(app);
    if (action >= MENU_FILE_NEW && action <= MENU_FILE_PROPERTIES) {
        app_files_menu_action(app, action);
        return;
    }
    if (action == MENU_SEARCH) {
        app->modal = MODAL_SEARCH;
        app->search_buf[0] = '\0';
        app->search_focus = 0;
        ui_set_input_active(true);
        ui_build_modal(app);
    } else if (action == MENU_SCAN) app_rescan(app);
    else if (action == MENU_ADD_ROOT) app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "",
                                                    "输入 ROM 根目录路径");
    else if (action == MENU_SORT) app_open_choice(app, CHOICE_SORT, "排序方式");
    else if (action == MENU_FILTER) app_open_choice(app, CHOICE_FILTER, "筛选游戏");
    else if (action == MENU_THEME) app_open_choice(app, CHOICE_THEME, "选择主题");
    else if (action == MENU_SETTINGS) {
        if (app->screen == SCREEN_SETTINGS) app_goto_screen(app, SCREEN_HOME);
        else app_goto_screen(app, SCREEN_SETTINGS);
    } else if (action == MENU_ADD_APP) {
        int idx = apps_add(&app->apps, "新应用", "true");
        if (idx >= 0) {
            app_set_app_category(app, "all");
            app->app_focus = app->app_filter_count > 0 ? app->app_filter_count - 1 : 0;
            app_open_app_editor(app, idx);
        }
    } else if (action == MENU_FILES) {
        if (app_files_available()) {
            app_goto_screen(app, SCREEN_FILES);
            return;
        }
        const char *env = getenv("TAIJI_FILES_BIN");
        char *path = env && *env ? xstrdup(env)
                                 : xstrdup("/usr/libexec/taiji/taiji-files");
        if (access(path, X_OK) == 0) {
            const char *argv[] = {path, "--mode", "manager", NULL};
            spawn_detached(argv);
        } else {
            ui_toast(app, "文件管理器未安装");
        }
        free(path);
    } else if (action == MENU_IMPORT_DESKTOP) {
        app_desktop_scan_start(app, true);
        ui_toast(app, "正在扫描桌面应用...");
    } else if (action == MENU_BACK) {
        app_goto_screen(app, SCREEN_HOME);
    } else if (action == MENU_HELP) {
        app->modal = MODAL_HELP;
        ui_build_modal(app);
    } else if (action == MENU_QUIT) {
        app_open_confirm(app, CONFIRM_QUIT, 0, "退出太极？", "游戏进度会先保存，随后返回桌面。");
    }
}

/* ── Scan ─────────────────────────────────────────────────────────────────── */

static void scan_progress(void *ud, const char *message, int percent)
{
    (void)ud; (void)message; (void)percent;
}

static void *library_scan_worker(void *ud)
{
    App *app = ud;
    app->scan_ok = library_scan(&app->scan_result, &app->config,
                                scan_progress, app);
    app->scan_done = true;
    return NULL;
}

void app_rescan(App *app)
{
    if (app->scan_running) return;
    library_init(&app->scan_result);
    app->scan_running = true;
    app->scan_done = false;
    app->scan_ok = false;
    app->modal = MODAL_SCAN;
    app->scan_pending = false;
    if (pthread_create(&app->scan_thread, NULL, library_scan_worker, app) != 0) {
        app->scan_running = false;
        library_free(&app->scan_result);
        app->modal = MODAL_NONE;
        ui_toast(app, "无法启动扫描");
        return;
    }
    ui_build_modal(app);
}

static void *desktop_scan_worker(void *ud)
{
    App *app = ud;
    DesktopImportStats st = desktop_import_apps(&app->desktop_scan_result);
    app->desktop_added = st.added;
    app->desktop_updated = st.updated;
    app->desktop_removed = st.removed;
    app->desktop_scan_done = true;
    return NULL;
}

static void app_desktop_scan_start(App *app, bool notify)
{
    if (app->desktop_scan_running) return;
    app->desktop_scan_result = app->apps;
    app->desktop_scan_notify = notify;
    app->desktop_added = app->desktop_updated = app->desktop_removed = 0;
    app->desktop_scan_done = false;
    app->desktop_scan_running = true;
    if (pthread_create(&app->desktop_scan_thread, NULL,
                       desktop_scan_worker, app) != 0) {
        app->desktop_scan_running = false;
        app->desktop_scan_done = false;
    }
}

static void app_desktop_scan_poll(App *app)
{
    if (!app->desktop_scan_running || !app->desktop_scan_done) return;
    pthread_join(app->desktop_scan_thread, NULL);
    app->desktop_scan_running = false;
    app->desktop_scan_done = false;
    app->apps = app->desktop_scan_result;
    memset(&app->desktop_scan_result, 0, sizeof(app->desktop_scan_result));
    int added = app->desktop_added;
    int updated = app->desktop_updated;
    int removed = app->desktop_removed;
    if (!added && !updated && !removed) {
        app->desktop_recheck_at = lv_tick_get() + 3000;
        return;
    }
    app_filter_apps(app);
    if (app->screen == SCREEN_APPS && app->modal == MODAL_NONE) {
        if (app->app_focus >= app->app_filter_count)
            app->app_focus = app->app_filter_count > 0 ?
                             app->app_filter_count - 1 : 0;
        app->app_badge = false;
        app->apps_refresh_pending = false;
        app_rebuild(app);
        ui_update_app_badge(app);
    } else if (added || updated) {
        app->app_badge = true;
        if (app->screen == SCREEN_APPS)
            app->apps_refresh_pending = true;
        ui_update_app_badge(app);
    }
    if (app->desktop_scan_notify) {
        char msg[160];
        snprintf(msg, sizeof(msg), "桌面应用：新增 %d / 更新 %d / 删除 %d",
                 added, updated, removed);
        ui_toast(app, msg);
    }
    app->desktop_recheck_at = lv_tick_get() + 3000;
}

static void app_scan_poll(App *app)
{
    if (!app->scan_running || !app->scan_done) return;
    pthread_join(app->scan_thread, NULL);
    app->scan_running = false;
    app->scan_done = false;
    library_free(&app->library);
    app->library = app->scan_result;
    memset(&app->scan_result, 0, sizeof(app->scan_result));
    app->modal = MODAL_NONE;
    ui_close_modal_visual(app);
    clamp_after_scan(app);
    app_rebuild(app);
    char msg[256];
    snprintf(msg, sizeof(msg), "扫描完成 / %d 个平台 / %d 个游戏",
             app->library.platform_count, app->library.game_count);
    ui_toast(app, msg);
}

/* ── Text edit helpers ────────────────────────────────────────────────────── */

static void utf8_pop(char *s)
{
    size_t n = strlen(s);
    if (n == 0) return;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s[i] = '\0';
}

static void text_append(App *app, const char *text)
{
    size_t cur = strlen(app->input_buf);
    size_t add = strlen(text);
    if (cur + add >= sizeof(app->input_buf)) add = sizeof(app->input_buf) - cur - 1;
    memcpy(app->input_buf + cur, text, add);
    app->input_buf[cur + add] = '\0';
    app->input_cursor = (int)strlen(app->input_buf);
}

static bool parse_hhmm(const char *s, int *out)
{
    int h = 0, m = 0;
    char tail = 0;
    if (!s || sscanf(s, "%d:%d%c", &h, &m, &tail) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = h * 60 + m;
    return true;
}

static void finish_text(App *app)
{
    if (app->text_purpose == TEXT_EDITOR_LINE) {
        int idx = app->modal_arg;
        editor_set_line(app, idx, app->input_buf);
        app->editor.focus = idx;
        app->editor.col = (int)strlen(app->input_buf);
        app->editor.want_col = app->editor.col;
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
        app_rebuild(app);
        editor_edit_line(app, idx + 1);
        return;
    }
    if (app->text_purpose == TEXT_ROOT) {
        char *abs = path_absolute(app->input_buf);
        if (!dir_exists(abs)) {
            ui_toast(app, "目录不存在，请检查路径");
            free(abs);
            return;
        }
        config_add_root(&app->config, abs);
        free(abs);
        app_save_settings(app);
        app_close_modal(app);
        ui_toast(app, "目录已添加，正在扫描...");
        app_rescan(app);
    } else if (app->text_purpose == TEXT_FILE_NAME) {
        app_files_text_done(app);
        return;
    } else if (app->text_purpose >= TEXT_SMB_HOST &&
               app->text_purpose <= TEXT_SMB_PASS) {
        app_files_smb_text_done(app);
        return;
    } else if (app->text_purpose == TEXT_SMB_SHARE_PATH ||
               app->text_purpose == TEXT_SMB_SHARE_NAME) {
        app_files_share_text_done(app);
        return;
    } else if (app->text_purpose == TEXT_SMB_AUTH_USER ||
               app->text_purpose == TEXT_SMB_AUTH_PASS) {
        app_files_smb_auth_done(app);
        return;
    } else if (app->text_purpose == TEXT_OOBE_ROM) {
        app_oobe_rom_done(app);
        return;
    } else if (app->text_purpose == TEXT_COMMAND) {
        char pending[80];
        snprintf(pending, sizeof(pending), "%s", app->core_pending_game_id);
        if (app->modal_arg >= 0 && app->modal_arg < app->library.platform_count) {
            Platform *p = &app->library.platforms[app->modal_arg];
            config_set_command(&app->config, p->id, app->input_buf);
            app_save_settings(app);
            app_close_modal(app);
            app_rebuild(app);
            if (pending[0]) {
                Game *g = library_find_game(&app->library, pending);
                if (g) {
                    app_launch_game_now(app, g, NULL);
                    return;
                }
            }
            ui_toast(app, "启动命令已保存");
        } else app_close_modal(app);
    } else if (app->text_purpose == TEXT_GAME_COMMAND) {
        Game *g = library_find_game(&app->library, app->selected_game_id);
        if (g) {
            free(g->command);
            g->command = xstrdup(app->input_buf);
            library_save_cache(&app->library);
        }
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "游戏启动命令已保存");
    } else if (app->text_purpose == TEXT_WIFI_PASSWORD) {
        char ssid[128];
        snprintf(ssid, sizeof(ssid), "%s", app->wifi_connect_ssid);
        char password[2048];
        snprintf(password, sizeof(password), "%s", app->input_buf);
        app_close_modal(app);
        app_do_wifi_connect(app, ssid, password);
        return;
    } else if (app->text_purpose == TEXT_TIMEZONE) {
        const char *raw = str_trim(app->input_buf);
        if (!*raw) {
            app_close_modal(app);
            return;
        }
        char error[256] = {0};
        if (!system_timezone_set(raw, error, sizeof(error))) {
            ui_toast(app, error[0] ? error : "时区设置失败");
            return;
        }
        app_close_modal(app);
        app_rebuild(app);
        char msg[256];
        if (i18n_language() == 1)
            snprintf(msg, sizeof(msg), "Time zone set to %s", raw);
        else
            snprintf(msg, sizeof(msg), "时区已设置为 %s", raw);
        ui_toast(app, msg);
    } else if (app->text_purpose == TEXT_AUTO_DARK ||
               app->text_purpose == TEXT_AUTO_LIGHT) {
        const char *raw = str_trim(app->input_buf);
        int minutes = -1;
        if (*raw && !parse_hhmm(raw, &minutes)) {
            ui_toast(app, "时间格式应为 HH:MM");
            return;
        }
        bool dark = app->text_purpose == TEXT_AUTO_DARK;
        if (*raw) {
            if (dark) app->config.auto_dark_min = minutes;
            else app->config.auto_light_min = minutes;
            if (dark && app->config.auto_light_min < 0)
                app->config.auto_light_min = 7 * 60;
            if (!dark && app->config.auto_dark_min < 0)
                app->config.auto_dark_min = 20 * 60;
        } else {
            app->config.auto_dark_min = -1;
            app->config.auto_light_min = -1;
        }
        app_save_settings(app);
        app_close_modal(app);
        app_apply_theme(app, false);
        app_rebuild(app);
        ui_toast(app, app->config.auto_dark_min < 0 ?
                        "已恢复默认 19:00 / 07:00" : "自动切换时间已保存");
    } else if (app->text_purpose == TEXT_APP_NAME ||
               app->text_purpose == TEXT_APP_COMMAND ||
               app->text_purpose == TEXT_APP_ICON) {
        int idx = app->app_edit_index;
        if (idx >= 0 && idx < app->apps.count) {
            AppEntry *a = &app->apps.apps[idx];
            if (app->text_purpose == TEXT_APP_NAME) {
                snprintf(a->name, sizeof(a->name), "%.*s", (int)sizeof(a->name) - 1,
                         app->input_buf[0] ? app->input_buf : a->name);
            } else if (app->text_purpose == TEXT_APP_COMMAND) {
                snprintf(a->command, sizeof(a->command), "%.*s",
                         (int)sizeof(a->command) - 1, app->input_buf);
            } else {
                snprintf(a->icon, sizeof(a->icon), "%.*s",
                         (int)sizeof(a->icon) - 1, app->input_buf);
            }
            apps_save(&app->apps);
        }
        app_close_modal(app);
        app->modal = MODAL_APP;
        app->app_edit_from_text = true;
        ui_build_modal(app);
        app_rebuild(app);
    }
}

/* ── Confirm execution ────────────────────────────────────────────────────── */

static void run_confirm(App *app)
{
    if (app->confirm_purpose == CONFIRM_QUIT) {
        app->backend.running = false;
        return;
    }
    if (app->confirm_purpose == CONFIRM_REMOVE_ROOT) {
        if (app->modal_arg >= 0 && app->modal_arg < app->config.root_count) {
            config_remove_root(&app->config, app->modal_arg);
            app_save_settings(app);
        }
        app->modal = MODAL_ROOTS;
        app->roots_focus = 0;
        ui_build_modal(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_RESCAN) {
        app_rescan(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_EDITOR_DISCARD) {
        editor_leave(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_RERUN_OOBE) {
        oobe_start(app, true);
        return;
    }
    if (app->confirm_purpose == CONFIRM_RESET_TAIJI) {
        app_reset_settings(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_RESET_MAP) {
        app->config.custom_mapping = false;
        app->config.swap_ab = app->config.swap_xy = false;
        app_save_settings(app);
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "已恢复默认按键");
        return;
    }
    if (app->confirm_purpose == CONFIRM_FILE_DELETE_PERM) {
        app_close_modal(app);
        app_files_delete_permanent(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_PASTE_OVERWRITE) {
        app_close_modal(app);
        app_files_paste_now(app);
        return;
    }
    if (app->confirm_purpose == CONFIRM_PAUSE_LIMIT) {
        if (app->pending_evict_id[0]) {
            const char *env = getenv("TAIJI_CLOSE_HELPER");
            char *path = env && *env ? xstrdup(env)
                                     : xstrdup("/usr/libexec/taiji/taiji-close-task");
            if (access(path, X_OK) == 0) {
                const char *argv[] = {path, app->pending_evict_id, NULL};
                spawn_detached(argv);
            }
            free(path);
        }
        AppEntry *entry = app->pending_app_id[0]
            ? apps_find(&app->apps, app->pending_app_id) : NULL;
        app->pending_app_id[0] = '\0';
        app->pending_evict_id[0] = '\0';
        app->modal = MODAL_NONE;
        app->modal_closing = false;
        app->control_closing = false;
        ui_set_input_active(false);
        ui_close_modal_visual(app);
        if (entry) {
            for (int i = 0; i < app->app_filter_count; i++) {
                if (strcmp(app->apps.apps[app->app_filter[i]].id, entry->id) == 0) {
                    app->app_focus = i;
                    break;
                }
            }
            launch_external_entry(app, entry);
        } else {
            ui_toast(app, "没有找到待启动的应用");
        }
        return;
    }
    if (app->confirm_purpose == CONFIRM_DELETE_APP) {
        int idx = app->modal_arg;
        if (idx >= 0 && idx < app->apps.count) {
            apps_remove(&app->apps, idx);
            app_filter_apps(app);
        }
        app->app_edit_index = -1;
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "应用已删除");
    }
}

/* ── Modal input ──────────────────────────────────────────────────────────── */

static void search_open_selected(App *app)
{
    GameList list = library_query(&app->library, "all", SORT_NAME, FILTER_ALL, app->search_buf);
    if (list.count == 0) { game_list_free(&list); return; }
    int idx = app->search_focus;
    if (idx < 0) idx = 0;
    if (idx >= list.count) idx = list.count - 1;
    char id[80];
    snprintf(id, sizeof(id), "%s", list.items[idx]->id);
    game_list_free(&list);
    app_close_modal(app);
    app_open_game(app, id);
}

static void modal_text_input(App *app, InputAction action, const char *text)
{
    if (action == IN_TEXT && text) { text_append(app, text); ui_refresh_modal(app); return; }
    if (action == IN_BACKSPACE) { utf8_pop(app->input_buf); app->input_cursor = (int)strlen(app->input_buf); ui_refresh_modal(app); return; }
    if ((action == IN_Y || action == IN_START) &&
        app->text_purpose != TEXT_EDITOR_LINE && app_files_pick_supported(app)) {
        app_files_pick_start(app);
        return;
    }
    if (action == IN_CONFIRM) { finish_text(app); return; }
    if (action == IN_CANCEL) {
        if (app->text_purpose == TEXT_EDITOR_LINE) {
            editor_set_line(app, app->modal_arg, app->input_buf);
            app->editor.focus = app->modal_arg;
            app->editor.col = (int)strlen(app->input_buf);
            app->editor.want_col = app->editor.col;
            external_osk_hide(app);
            app->modal = MODAL_NONE;
            app->modal_closing = false;
            ui_set_input_active(false);
            ui_close_modal_visual(app);
            app_rebuild(app);
            return;
        }
        app_close_modal(app);
        return;
    }
}

static void modal_search_input(App *app, InputAction action, const char *text)
{
    if (action == IN_TEXT && text) {
        size_t cur = strlen(app->search_buf);
        size_t add = strlen(text);
        if (cur + add >= sizeof(app->search_buf)) add = sizeof(app->search_buf) - cur - 1;
        memcpy(app->search_buf + cur, text, add);
        app->search_buf[cur + add] = '\0';
        app->search_focus = 0;
        ui_refresh_modal(app);
        return;
    }
    if (action == IN_BACKSPACE) { utf8_pop(app->search_buf); app->search_focus = 0; ui_refresh_modal(app); return; }
    if (action == IN_UP) { if (app->search_focus > 0) app->search_focus--; ui_refresh_modal(app); return; }
    if (action == IN_DOWN) {
        GameList list = library_query(&app->library, "all", SORT_NAME, FILTER_ALL, app->search_buf);
        int max = list.count < 7 ? list.count : 7;
        if (app->search_focus + 1 < max) app->search_focus++;
        game_list_free(&list);
        ui_refresh_modal(app);
        return;
    }
    if (action == IN_CONFIRM) { search_open_selected(app); return; }
    if (action == IN_CANCEL) { app_close_modal(app); return; }
}

static void modal_input(App *app, InputAction action, const char *text)
{
    if (action == IN_FULLSCREEN) { backend_toggle_fullscreen(&app->backend); return; }
    if (app->modal == MODAL_OOBE_DATE) { app_oobe_date_input(app, action); return; }
    if (app->modal == MODAL_TEXT) { modal_text_input(app, action, text); return; }
    if (app->modal == MODAL_APP) { modal_app_input(app, action); return; }
    if (app->modal == MODAL_CONTROL) {
        if (action == IN_CANCEL || action == IN_B || action == IN_START) {
            app_close_modal(app);
            return;
        }
        if (app->control_view == STATUS_BATTERY &&
            (app->perf.available || app->gpu.available)) {
            bool has_cpu = app->perf.available;
            bool has_gpu = app->gpu.available;
            int modules = (has_cpu ? 1 : 0) + (has_gpu ? 1 : 0);
            if (modules > 1 && (action == IN_UP || action == IN_DOWN)) {
                app->power_module = app->power_module ? 0 : 1;
                if (app->power_module == 1 && !has_gpu) app->power_module = 0;
                if (app->power_module == 0 && !has_cpu) app->power_module = 1;
                app->control_focus = app->power_module == 1 ?
                    mode_index_of(app->gpu.modes, app->gpu.mode) :
                    mode_index_of(app->perf.modes, app->perf.mode);
                ui_refresh_control_modal(app);
                return;
            }
            int count = app->power_module == 1 ? gpu_mode_count(&app->gpu) :
                                                 perf_mode_count(&app->perf);
            if (count > 0) {
                if (action == IN_LEFT) {
                    if (app->control_focus > 0) app->control_focus--;
                    ui_refresh_control_modal(app);
                    return;
                }
                if (action == IN_RIGHT) {
                    if (app->control_focus + 1 < count) app->control_focus++;
                    ui_refresh_control_modal(app);
                    return;
                }
                if (action == IN_CONFIRM || action == IN_A) {
                    if (app->power_module == 1) {
                        int mode = gpu_mode_at(&app->gpu, app->control_focus);
                        if (mode != GPU_MODE_NONE) app_control_set_gpu(app, mode);
                    } else {
                        int mode = perf_mode_at(&app->perf, app->control_focus);
                        if (mode != PERF_MODE_NONE) app_control_set_perf(app, mode);
                    }
                    return;
                }
            }
        }
        if (app->control_view == STATUS_VOLUME) {
            if (action == IN_LEFT || action == IN_DOWN)
                app_control_volume(app, -5);
            else if (action == IN_RIGHT || action == IN_UP)
                app_control_volume(app, 5);
            return;
        }
        if (app->control_view == STATUS_WIFI) {
            int count = app->control_expanded ?
                (app->control_radio == 1 ? app->bt.count : app->wifi.count) : 0;
            if (action == IN_LEFT || action == IN_RIGHT) {
                /* 左右切换左侧无线列表 / 右侧设备列表的焦点。 */
                app->control_col = app->control_col ? 0 : 1;
                if (app->control_col == 1) {
                    if (app->control_focus >= count)
                        app->control_focus = count > 0 ? count - 1 : 0;
                    if (count > 0) app->control_expanded = true;
                    if (app->control_radio == 1)
                        bluetooth_scan_async(true);
                }
                ui_refresh_control_modal(app);
            }
            else if (action == IN_UP || action == IN_DOWN) {
                if (app->control_col == 0) {
                    /* 左侧列表：上下切换 Wi-Fi / 蓝牙，右侧同步换列表。 */
                    app->control_radio = app->control_radio ? 0 : 1;
                    app->control_focus = 0;
                    app->control_expanded = true;
                    if (app->control_radio == 1) {
                        snprintf(app->bt.message, sizeof(app->bt.message), "正在扫描蓝牙...");
                        bluetooth_scan_async(true);
                    }
                } else if (count > 0) {
                    if (action == IN_UP) {
                        if (app->control_focus > 0) app->control_focus--;
                    } else if (app->control_focus + 1 < count) {
                        app->control_focus++;
                    }
                }
                ui_refresh_control_modal(app);
            }
            else if (action == IN_CONFIRM || action == IN_A) app_control_activate(app);
            else if (action == IN_X) {
                if (app->control_radio == 1) app_control_toggle_bluetooth(app);
                else app_control_toggle_wifi(app);
            }
            else if (action == IN_Y) app_control_rescan_wifi(app);
        }
        return;
    }
    if (app->modal == MODAL_GAMEPAD) {
        if (action == IN_CANCEL || action == IN_B) app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_POWER) {
        if (action == IN_UP) {
            if (app->power_focus > 0) app->power_focus--;
            ui_update_power_focus(app);
        } else if (action == IN_DOWN) {
            if (app->power_focus < 3) app->power_focus++;
            ui_update_power_focus(app);
        } else if (action == IN_CONFIRM || action == IN_A) {
            app_power_action(app, app->power_focus);
        } else if (action == IN_CANCEL || action == IN_B || action == IN_START) {
            app_close_modal(app);
        }
        return;
    }
    if (app->modal == MODAL_SEARCH) { modal_search_input(app, action, text); return; }
    if (app->modal == MODAL_CONFIRM) {
        if (action == IN_LEFT || action == IN_RIGHT) { app->confirm_focus = 1 - app->confirm_focus; ui_refresh_modal(app); }
        else if (action == IN_CONFIRM) { if (app->confirm_focus == 0) run_confirm(app); else app_close_modal(app); }
        else if (action == IN_CANCEL) app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_CHOICE) {
        int values[64], n = 0;
        if (app->choice_purpose == CHOICE_THEME) {
            n = theme_preset_count() + 1;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_COLUMNS) {
            int vals[] = {0, 4, 5, 6, 7, 8};
            n = 6;
            for (int i = 0; i < n; i++) values[i] = vals[i];
        } else if (app->choice_purpose == CHOICE_SORT) {
            n = 4;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_LANGUAGE) {
            n = 2;
            values[0] = 0;
            values[1] = 1;
        } else if (app->choice_purpose == CHOICE_RENDERER) {
            n = 3;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_EXTRACT_TARGET) {
            n = 2;
            values[0] = 0;
            values[1] = 1;
        } else if (app->choice_purpose == CHOICE_OPEN_WITH) {
            n = app->files_open_with_count + (app->files_open_with_builtin ? 1 : 0);
            if (n > 64) n = 64;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_ARCHIVE_FORMAT) {
            n = 3;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_TIMEZONE) {
            n = app_timezone_option_count(app);
            if (n > 64) n = 64;
            for (int i = 0; i < n; i++) values[i] = i;
        } else if (app->choice_purpose == CHOICE_UI_SCALE) {
            int max = app_ui_scale_max(app);
            n = app_ui_scale_values(values, 16, max);
        } else if (app->choice_purpose == CHOICE_CORE) {
            n = app->core_item_count;
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            for (int i = 0; i < n; i++) values[i] = i;
        } else {
            n = 4;
            for (int i = 0; i < n; i++) values[i] = i;
        }
        if (app->choice_purpose == CHOICE_OPEN_WITH && action == IN_Y) {
            int display = app->choice_focus;
            if (display >= 0 && display < app->files_open_with_count) {
                int app_index = app->files_open_with_apps[display];
                app_close_modal(app);
                app_files_open_with(app, app_index, true);
            }
            return;
        }
        if (action == IN_UP) { app->choice_focus = (app->choice_focus - 1 + n) % n; ui_refresh_modal(app); }
        else if (action == IN_DOWN) { app->choice_focus = (app->choice_focus + 1) % n; ui_refresh_modal(app); }
        else if (action == IN_CONFIRM) app_do_choice(app, app->choice_focus, values[app->choice_focus]);
        else if (action == IN_CANCEL) {
            if (app->choice_purpose == CHOICE_CORE && app->core_picker_all)
                core_picker_show_compatible(app);
            else
                app_close_modal(app);
        }
        return;
    }
    if (app->modal == MODAL_MENU) {
        if (app->menu_info) {
            if (action == IN_CONFIRM || action == IN_A || action == IN_CANCEL ||
                action == IN_B || action == IN_Y || action == IN_START)
                app_close_modal(app);
            return;
        }
        const char *labels[20];
        MenuAction acts[20];
        int n = 0;
        app_menu_labels(app, labels, acts, &n);
        if (action == IN_UP) { app->menu_focus = (app->menu_focus - 1 + n) % n; ui_refresh_modal(app); }
        else if (action == IN_DOWN) { app->menu_focus = (app->menu_focus + 1) % n; ui_refresh_modal(app); }
        else if (action == IN_CONFIRM) { if (app->menu_focus >= 0 && app->menu_focus < n) app_run_menu_action(app, acts[app->menu_focus]); }
        else if (action == IN_CANCEL) app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_ROOTS) {
        if (action == IN_UP) { if (app->roots_focus > 0) app->roots_focus--; ui_refresh_modal(app); }
        else if (action == IN_DOWN) { if (app->roots_focus + 1 < app->config.root_count) app->roots_focus++; ui_refresh_modal(app); }
        else if (action == IN_CONFIRM) {
            if (app->config.root_count > 0)
                app_open_confirm(app, CONFIRM_REMOVE_ROOT, app->roots_focus,
                                 "移除这个目录？", app->config.roots[app->roots_focus]);
        } else if (action == IN_CANCEL) app_close_modal(app);
        else if (action == IN_X) {
            app_close_modal(app);
            app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "", "输入 ROM 根目录路径");
        }
        return;
    }
    if (app->modal == MODAL_MAPPING) {
        if (action == IN_CANCEL) {
            backend_set_capture(false);
            app_close_modal(app);
        }
        return;
    }
    if (app->modal == MODAL_DETAIL) {
        Game *g = library_find_game(&app->library, app->selected_game_id);
        if (action == IN_CONFIRM) { if (g) app_launch_game(app, g); }
        else if (action == IN_CANCEL) app_close_modal(app);
        else if (action == IN_Y && g) app_toggle_favorite(app, g);
        else if (action == IN_X && g) {
            app_close_modal(app);
            app_open_text(app, TEXT_GAME_COMMAND, 0, "游戏启动命令", g->command,
                          "留空则使用平台默认命令。%ROM% 表示游戏路径。");
        }
        return;
    }
    if (app->modal == MODAL_HELP) {
        if (action == IN_UP && app->ui.modal_scroll)
            lv_obj_scroll_by(app->ui.modal_scroll, 0, 40, LV_ANIM_OFF);
        else if (action == IN_DOWN && app->ui.modal_scroll)
            lv_obj_scroll_by(app->ui.modal_scroll, 0, -40, LV_ANIM_OFF);
        else if (action == IN_CANCEL || action == IN_CONFIRM)
            app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_SMB_SHARE) {
        SmbUi *ui = &app->smb_ui;
        if (action == IN_UP || action == IN_DOWN) {
            ui->edit_focus = ui->edit_focus ? 0 : 1;
            ui_refresh_modal(app);
        }
        else if (action == IN_CONFIRM || action == IN_A) {
            if (ui->edit_index < 0) {
                if (ui->edit_focus == 0) {
                    ui->pending_writable = !ui->pending_writable;
                    ui_refresh_modal(app);
                } else {
                    const char *base = strrchr(ui->pending_path, '/');
                    base = base ? base + 1 : ui->pending_path;
                    char name[96];
                    snprintf(name, sizeof(name), "%s", *base ? base : "share");
                    char err[192] = "";
                    if (smb_local_add(app, name, ui->pending_path,
                                      ui->pending_writable, err, sizeof(err))) {
                        ui->edit_index = -1;
                        app_close_modal(app);
                        app_rebuild(app);
                        ui_toast(app, "共享已创建，可在「管理」中开启服务");
                    } else {
                        ui_toast(app, err[0] ? err : "创建共享失败");
                    }
                }
                return;
            }
            if (ui->edit_index >= 0 && ui->edit_index < ui->local_count) {
                SmbLocalShare *sh = &ui->local[ui->edit_index];
                char err[192] = "";
                if (ui->edit_focus == 0) {
                    bool next = !sh->writable;
                    if (smb_local_set(app, sh->name, sh->path, next, err, sizeof(err)))
                        ui_toast(app, next ? "已设为访客读写" : "已设为访客只读");
                    else
                        ui_toast(app, err[0] ? err : "保存共享设置失败");
                    ui_refresh_modal(app);
                } else {
                    if (smb_local_remove(app, sh->name, err, sizeof(err))) {
                        ui->edit_index = -1;
                        app_close_modal(app);
                        app_rebuild(app);
                        ui_toast(app, "共享已删除");
                    } else {
                        ui_toast(app, err[0] ? err : "删除共享失败");
                    }
                }
            }
            return;
        }
        else if (action == IN_CANCEL || action == IN_B) app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_EDITOR_HELP) {
        if (action == IN_CANCEL || action == IN_CONFIRM ||
            action == IN_A || action == IN_B || action == IN_SELECT)
            app_close_modal(app);
        return;
    }
    if (app->modal == MODAL_SCAN) return;
}

/* ── Main input ───────────────────────────────────────────────────────────── */

static void switch_tab(App *app, int delta)
{
    if (app->files_pick != FILES_PICK_NONE) return;
    int tabs = app->ui.nav_count;
    if (tabs <= 0) return;
    int tab = nav_index_for_screen(app);
    int from_tab = tab;
    tab = (tab + delta + tabs) % tabs;
    Screen target = nav_screen_at(app, tab);
    taiji_debug_log("switch_tab screen=%d target=%d delta=%d index=%d->%d/%d",
                    (int)app->screen, (int)target, delta, from_tab, tab, tabs);
    app_goto_screen(app, target);
    audio_play(SND_NAV);
}

static void move_game_focus(App *app, int delta)
{
    GameList list = library_query(&app->library, screen_collection(app),
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    if (list.count == 0) { game_list_free(&list); return; }
    int idx = app->game_focus + delta;
    if (idx < 0) idx = 0;
    if (idx >= list.count) idx = list.count - 1;
    if (idx != app->game_focus) {
        app->game_focus = idx;
        audio_play(SND_NAV);
        int page = idx / game_page_size(app);
        if (page != app->game_page) app_rebuild(app);
        else ui_update_games(app);
    }
    game_list_free(&list);
}

static void cycle_collection(App *app, int delta)
{
    Collection cols[128];
    int n = app_collections(app, cols, 128);
    if (n <= 1) return;
    const char *cur = screen_collection(app);
    int idx = 0;
    for (int i = 0; i < n; i++) if (strcmp(cols[i].id, cur) == 0) idx = i;
    idx = (idx + delta + n) % n;
    app_enter_collection(app, cols[idx].id);
    audio_play(SND_NAV);
}

static void input_overview(App *app, InputAction action)
{
    int n = app->tasks.count;
    if (n <= 0) {
        if (action == IN_CANCEL || action == IN_B || action == IN_START)
            app_close_overview(app);
        return;
    }
    int focus = app->overview_focus;
    if (action == IN_LEFT || action == IN_LT) {
        focus = (focus - 1 + n) % n;
    } else if (action == IN_RIGHT || action == IN_RT) {
        focus = (focus + 1) % n;
    } else if (action == IN_X) {
        TaskEntry *t = &app->tasks.items[focus];
        if (t->kind == TASK_KIND_TAIJI) {
            app->overview_kill_armed = false;
            ui_toast(app, "不能关闭太极");
            return;
        }
        uint32_t now = lv_tick_get();
        if (app->overview_kill_armed && app->overview_kill_index == focus &&
            now - app->overview_kill_at < 4000) {
            app->overview_kill_armed = false;
            audio_play(SND_BACK);
            app_close_task(app, focus);
        } else {
            app->overview_kill_armed = true;
            app->overview_kill_index = focus;
            app->overview_kill_at = now;
            char msg[256];
            char hint[96];
            ui_button_hint(app, hint, sizeof(hint), "再按一次 {X} 关闭");
            snprintf(msg, sizeof(msg), "%s %s", hint, t->title);
            ui_toast(app, msg);
            audio_play(SND_NAV);
        }
        return;
    } else if (action == IN_CONFIRM || action == IN_A) {
        app->overview_kill_armed = false;
        audio_play(SND_ENTER);
        app_activate_task(app, focus);
        return;
    } else if (action == IN_CANCEL || action == IN_B || action == IN_START) {
        app->overview_kill_armed = false;
        audio_play(SND_BACK);
        app_close_overview(app);
        return;
    } else {
        return;
    }
    if (focus != app->overview_focus) {
        bool next = (action == IN_RIGHT || action == IN_RT);
        app->overview_slide_dir = next ? 1 : -1;
        app->overview_focus = focus;
        app->overview_kill_armed = false;
        audio_play(SND_NAV);
        app_rebuild(app);
    }
}

static void input_home(App *app, InputAction action){
    int n = app_total_collections(app);
    int per_page = app->layout.home_page > 0 ? app->layout.home_page : 8;
    if (n <= 0 && (action == IN_CONFIRM || action == IN_A)) {
        audio_play(SND_ENTER);
        app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "",
                      "输入 ROM 根目录路径，例如 ~/ROMs 或 /Volumes/Games");
        return;
    }
    if (n > 0) {
        int cols = app->layout.home_cols > 0 ? app->layout.home_cols : 4;
        int target = app->home_focus;
        if (action == IN_LEFT && target % cols > 0) target--;
        else if (action == IN_RIGHT && (target % cols) < cols - 1 && target + 1 < n) target++;
        else if (action == IN_UP && target >= cols) target -= cols;
        else if (action == IN_DOWN) {
            if (target + cols < n) target += cols;
            else if (target < n - 1) target = n - 1;
        } else if (action == IN_LT) target -= per_page;
        else if (action == IN_RT) target += per_page;
        if (target < 0) target = 0;
        if (target >= n) target = n - 1;
        if (target != app->home_focus) {
            app->home_focus = target;
            audio_play(SND_NAV);
            ui_update_home(app);
            return;
        }
    }
    if (action == IN_CONFIRM || action == IN_A) {
        Collection cols[128];
        int n2 = app_collections(app, cols, 128);
        if (app->home_focus >= 0 && app->home_focus < n2) {
            app_enter_collection(app, cols[app->home_focus].id);
            audio_play(SND_ENTER);
        }
    } else if (action == IN_X) {
        app->modal = MODAL_SEARCH;
        app->search_buf[0] = '\0';
        app->search_focus = 0;
        ui_set_input_active(true);
        ui_build_modal(app);
    } else if (action == IN_Y || action == IN_START) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
    } else if (action == IN_SELECT) {
        app->modal = MODAL_HELP;
        ui_build_modal(app);
    } else if (action == IN_LB) switch_tab(app, -1);
    else if (action == IN_RB) switch_tab(app, 1);
}

static void input_games(App *app, InputAction action)
{
    GameList list = library_query(&app->library, screen_collection(app),
                                  (SortMode)app->sort_mode, (FilterMode)app->filter_mode, NULL);
    int count = list.count;
    int cols = game_columns(app);
    game_list_free(&list);
    if (action == IN_LEFT) move_game_focus(app, -1);
    else if (action == IN_RIGHT) move_game_focus(app, 1);
    else if (action == IN_UP) move_game_focus(app, -cols);
    else if (action == IN_DOWN) move_game_focus(app, cols);
    else if (action == IN_LT) cycle_collection(app, -1);
    else if (action == IN_RT) cycle_collection(app, 1);
    else if (action == IN_CONFIRM || action == IN_A) {
        if (count == 0) {
            audio_play(SND_ENTER);
            app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "",
                          "输入 ROM 根目录路径，例如 ~/ROMs 或 /Volumes/Games");
        } else {
            Game *g = app_selected_game(app);
            if (g) { audio_play(SND_ENTER); app_launch_game(app, g); }
        }
    } else if (action == IN_CANCEL || action == IN_B) {
        audio_play(SND_BACK);
    } else if (action == IN_X) {
        app->modal = MODAL_DETAIL;
        ui_build_modal(app);
        audio_play(SND_NAV_STRONG);
    } else if (action == IN_Y) {
        Game *g = app_selected_game(app);
        if (g) app_toggle_favorite(app, g);
    } else if (action == IN_START) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
    } else if (action == IN_SELECT) {
        app->modal = MODAL_HELP;
        ui_build_modal(app);
    } else if (action == IN_LB) switch_tab(app, -1);
    else if (action == IN_RB) switch_tab(app, 1);
    else if (action == IN_UP) {
        if (count == 0) {}
    }
}

static int app_category_count(App *app, const char *id)
{
    int n = 0;
    for (int i = 0; i < app->apps.count; i++) {
        if (apps_category_matches(&app->apps.apps[i], id)) n++;
    }
    return n;
}

static void input_apps(App *app, InputAction action)
{
    int count = app->app_filter_count;
    int cols = app->layout.app_cols > 0 ? app->layout.app_cols : 3;
    int cats = apps_category_count();
    int focus_before = app->app_focus;

    if (app->app_on_categories) {
        if (action == IN_LEFT) {
            app->app_cat_focus = (app->app_cat_focus - 1 + cats) % cats;
            app_rebuild(app);
            audio_play(SND_NAV);
            return;
        }
        if (action == IN_RIGHT) {
            app->app_cat_focus = (app->app_cat_focus + 1) % cats;
            app_rebuild(app);
            audio_play(SND_NAV);
            return;
        }
        if (action == IN_CONFIRM || action == IN_A || action == IN_DOWN) {
            app_set_app_category(app, apps_category_id(app->app_cat_focus));
            audio_play(SND_NAV_STRONG);
            return;
        }
        if (action == IN_CANCEL || action == IN_B) {
            app->app_on_categories = false;
            app_rebuild(app);
            return;
        }
        if (action == IN_Y || action == IN_START) {
            int idx = apps_add(&app->apps, "新应用", "true");
            if (idx >= 0) {
                app_set_app_category(app, "all");
                app->app_focus = app->app_filter_count > 0 ? app->app_filter_count - 1 : 0;
                app_open_app_editor(app, idx);
            }
            return;
        }
        return;
    }

    if (action == IN_UP) {
        if (app->app_focus < cols) {
            app->app_on_categories = true;
            app->app_cat_focus = apps_category_index(app->app_cat);
            app_rebuild(app);
            audio_play(SND_NAV);
            return;
        }
        app->app_focus -= cols;
    }
    if (action == IN_LT || action == IN_RT) {
        int step = action == IN_LT ? -1 : 1;
        int cat = apps_category_index(app->app_cat);
        int chosen = cat;
        for (int i = 0; i < cats; i++) {
            cat = (cat + step + cats) % cats;
            if (app_category_count(app, apps_category_id(cat)) > 0) {
                chosen = cat;
                break;
            }
        }
        app_set_app_category(app, apps_category_id(chosen));
        audio_play(SND_NAV);
        return;
    }
    if (action == IN_LEFT && app->app_focus > 0) app->app_focus--;
    else if (action == IN_RIGHT && app->app_focus + 1 < count) app->app_focus++;
    else if (action == IN_DOWN) {
        /* If the next row exists but the same column is empty (last row is
         * partial), land on the last item of that row instead of doing
         * nothing. */
        int row = app->app_focus / cols;
        int next_start = (row + 1) * cols;
        if (next_start < count) {
            int target = app->app_focus + cols;
            if (target >= count) {
                int next_end = next_start + cols - 1;
                if (next_end >= count) next_end = count - 1;
                target = next_end;
            }
            app->app_focus = target;
        }
    }
    else if (action == IN_CONFIRM || action == IN_A) {
        audio_play(SND_ENTER);
        app_launch_external(app);
        return;
    } else if (action == IN_X) {
        int idx = app_selected_app_index(app);
        if (idx >= 0) app_open_app_editor(app, idx);
        return;
    } else if (action == IN_Y) {
        int idx = apps_add(&app->apps, "新应用", "true");
        if (idx >= 0) {
            app_set_app_category(app, "all");
            app->app_focus = app->app_filter_count > 0 ? app->app_filter_count - 1 : 0;
            app_open_app_editor(app, idx);
        }
        return;
    } else if (action == IN_CANCEL || action == IN_B) {
        audio_play(SND_BACK);
        return;
    } else if (action == IN_START) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
        return;
    } else if (action == IN_SELECT) {
        app->modal = MODAL_HELP;
        ui_build_modal(app);
        return;
    }

    if (app->app_focus >= count) app->app_focus = count > 0 ? count - 1 : 0;
    if (app->app_focus < 0) app->app_focus = 0;
    taiji_debug_log("apps action=%d focus=%d count=%d cat=%s cols=%d",
                    (int)action, app->app_focus, count, app->app_cat, cols);
    if (app->app_focus != focus_before) {
        app->app_focus_want = app->app_focus;
        ui_update_apps(app);
        /* Render this step immediately. The display refresh timer runs much
         * slower than the input on this device, so without this several
         * moves collapse into one frame and the UI appears to jump back. */
        audio_play(SND_NAV);
    }
}

static void modal_app_input(App *app, InputAction action)
{
    if (app->app_edit_index < 0 || app->app_edit_index >= app->apps.count) {
        app_close_modal(app);
        return;
    }
    AppEntry *a = &app->apps.apps[app->app_edit_index];
    if (action == IN_UP) {
        if (app->app_edit_row > 0) app->app_edit_row--;
        audio_play(SND_NAV);
        ui_update_modal_app(app);
        return;
    }
    if (action == IN_DOWN) {
        if (app->app_edit_row < 10) app->app_edit_row++;
        audio_play(SND_NAV);
        ui_update_modal_app(app);
        return;
    }
    if (action == IN_CANCEL || action == IN_B) {
        apps_save(&app->apps);
        app_close_modal(app);
        app_rebuild(app);
        ui_toast(app, "应用设置已保存");
        return;
    }
    if (action == IN_CONFIRM || action == IN_A) {
        switch (app->app_edit_row) {
            case 0: app_open_text(app, TEXT_APP_NAME, 0, "应用名称", a->name, "显示在应用网格中的名称"); break;
            case 1: app_open_text(app, TEXT_APP_COMMAND, 0, "启动命令", a->command, "例如 mpv --fullscreen %FILE% 或 retroarch"); break;
            case 2: app_open_text(app, TEXT_APP_ICON, 0, "图标路径", a->icon, "PNG/JPG 路径，留空则使用文字图标"); break;
            case 3: {
                int cat = apps_cycle_category(a->category, 1);
                snprintf(a->category, sizeof(a->category), "%s", apps_category_id(cat));
                if (!a->on_return_custom)
                    a->on_return = strcmp(a->category, "media") == 0 ?
                                   APP_ON_RETURN_RUN : APP_ON_RETURN_PAUSE;
                apps_save(&app->apps);
                app_filter_apps(app);
                ui_update_modal_app(app);
                break;
            }
            case 4: a->mode = apps_cycle_mode(a->mode, 1); apps_save(&app->apps); ui_update_modal_app(app); break;
            case 5: a->on_return = apps_cycle_return(a->on_return, 1);
                a->on_return_custom = true;
                apps_save(&app->apps); ui_update_modal_app(app); break;
            case 6: a->backend = apps_cycle_backend(a->backend, 1); apps_save(&app->apps); ui_update_modal_app(app); break;
            case 7: a->heavy = !a->heavy; apps_save(&app->apps); ui_update_modal_app(app); break;
            case 8: a->background = !a->background; apps_save(&app->apps); ui_update_modal_app(app); break;
            case 9:
                a->terminal = (a->terminal + 1) % 3;
                a->terminal_custom = true;
                apps_save(&app->apps);
                ui_update_modal_app(app);
                break;
            case 10: app_open_confirm(app, CONFIRM_DELETE_APP, app->app_edit_index,
                                      "删除这个应用？", a->name); break;
        }
        return;
    }
    if (action == IN_LEFT || action == IN_RIGHT) {
        int d = action == IN_RIGHT ? 1 : -1;
        if (app->app_edit_row == 3) {
            int cat = apps_cycle_category(a->category, d);
            snprintf(a->category, sizeof(a->category), "%s", apps_category_id(cat));
            if (!a->on_return_custom)
                a->on_return = strcmp(a->category, "media") == 0 ?
                               APP_ON_RETURN_RUN : APP_ON_RETURN_PAUSE;
            app_filter_apps(app);
        }
        else if (app->app_edit_row == 4) a->mode = apps_cycle_mode(a->mode, d);
        else if (app->app_edit_row == 5) {
            a->on_return = apps_cycle_return(a->on_return, d);
            a->on_return_custom = true;
        }
        else if (app->app_edit_row == 6) a->backend = apps_cycle_backend(a->backend, d);
        else if (app->app_edit_row == 7) a->heavy = !a->heavy;
        else if (app->app_edit_row == 8) a->background = !a->background;
        else if (app->app_edit_row == 9) {
            a->terminal = (a->terminal + d + 3) % 3;
            a->terminal_custom = true;
        }
        apps_save(&app->apps);
        audio_play(SND_NAV);
        ui_update_modal_app(app);
        return;
    }
}

static void input_settings(App *app, InputAction action)
{
    if (app->settings_license) {
        if (action == IN_CANCEL || action == IN_B) {
            app->settings_license = false;
            app->settings_on_rail = false;
            app->settings_row = 3;
            app_rebuild(app);
            audio_play(SND_BACK);
        } else if (action == IN_UP) {
            if (app->ui.settings_license_body)
                lv_obj_scroll_by(app->ui.settings_license_body, 0, -48, LV_ANIM_ON);
        } else if (action == IN_DOWN) {
            if (app->ui.settings_license_body)
                lv_obj_scroll_by(app->ui.settings_license_body, 0, 48, LV_ANIM_ON);
        } else if (action == IN_LEFT) {
            if (app->ui.settings_license_body)
                lv_obj_scroll_by(app->ui.settings_license_body, 0, -180, LV_ANIM_ON);
        } else if (action == IN_RIGHT) {
            if (app->ui.settings_license_body)
                lv_obj_scroll_by(app->ui.settings_license_body, 0, 180, LV_ANIM_ON);
        }
        return;
    }
    if (app->settings_about) {
        if (action == IN_CANCEL || action == IN_B) {
            app->settings_about = false;
            app->settings_on_rail = false;
            app_rebuild(app);
            audio_play(SND_BACK);
        }
        return;
    }
    int cats = app_settings_rail_count();
    int total = 0;
    app_settings_rows(app, &total);

    if (app->settings_on_rail) {
        if (action == IN_UP || action == IN_DOWN) {
            int idx = app_settings_rail_index(app->settings_cat);
            idx = (idx + (action == IN_DOWN ? 1 : -1) + cats) % cats;
            app->settings_cat = app_settings_rail_cat(idx);
            app->settings_row = 0;
            app->settings_page = 0;
            app_rebuild(app);
            audio_play(SND_NAV);
        }
        else if (action == IN_CONFIRM || action == IN_A || action == IN_RIGHT) {
            if (total > 0) { app->settings_on_rail = false; app->settings_row = 0; ui_update_settings(app); audio_play(SND_NAV_STRONG); }
        } else if (action == IN_CANCEL || action == IN_B) {
            audio_play(SND_BACK);
        } else if (action == IN_Y || action == IN_START) {
            app->modal = MODAL_MENU;
            app->menu_focus = 0;
            ui_build_modal(app);
        } else if (action == IN_LB) switch_tab(app, -1);
        else if (action == IN_RB) switch_tab(app, 1);
        return;
    }

    if (action == IN_UP) {
        if (app->settings_row > 0) {
            app->settings_row--;
            ui_update_settings(app);
            audio_play(SND_NAV);
        }
    } else if (action == IN_DOWN) {
        if (app->settings_row + 1 < total) {
            app->settings_row++;
            ui_update_settings(app);
            audio_play(SND_NAV);
        }
    } else if (action == IN_LEFT) {
        app->settings_on_rail = true;
        ui_update_settings(app);
        audio_play(SND_NAV);
    } else if (action == IN_RIGHT) {
        /* 右侧内容区不再用左右键修改值。 */
    } else if (action == IN_CONFIRM || action == IN_A) {
        app_settings_activate(app, app->settings_row);
        audio_play(SND_NAV_STRONG);
    } else if (action == IN_CANCEL || action == IN_B) {
        app->settings_on_rail = true;
        ui_update_settings(app);
        audio_play(SND_BACK);
    } else if (action == IN_START || action == IN_Y) {
        app->modal = MODAL_MENU;
        app->menu_focus = 0;
        ui_build_modal(app);
    } else if (action == IN_LB) switch_tab(app, -1);
    else if (action == IN_RB) switch_tab(app, 1);
}

/* 没有左右分栏的页面，从首行可以直接进入顶部导航；
 * 有右侧内容区的页面，只有侧栏首项允许进入，右侧内容区顶部停住。 */
static bool app_at_top_edge(App *app)
{
    UiLayout *L = &app->layout;
    if (app->screen == SCREEN_HOME) {
        int cols = L->home_cols > 0 ? L->home_cols : 4;
        int per_page = L->home_page > 0 ? L->home_page : 8;
        return (app->home_focus % per_page) < cols;
    }
    if (app->screen == SCREEN_GAMES || app->screen == SCREEN_FAVORITES ||
        app->screen == SCREEN_RECENT) {
        int cols = L->game_cols > 0 ? L->game_cols : 4;
        int per_page = L->game_page > 0 ? L->game_page : 8;
        return (app->game_focus % per_page) < cols;
    }
    if (app->screen == SCREEN_APPS)
        return app->app_on_categories;
    if (app->screen == SCREEN_FILES) {
        if (app->files_pick != FILES_PICK_NONE || app->files_mtp_panel)
            return false;
        return app->files_on_sidebar && app->files_side_focus == 0;
    }
    if (app->screen == SCREEN_STORE) {
        if (app->store_settings || app->store_detail ||
            app->store_task_running || app->store_search_edit)
            return false;
        return app->store_on_sidebar && app->store_side_focus == 0;
    }
    if (app->screen == SCREEN_EDITOR)
        return app->editor.focus == 0;
    if (app->screen == SCREEN_OVERVIEW)
        return true;
    if (app->screen == SCREEN_SETTINGS) {
        if (app->settings_about || app->settings_license) return false;
        return app->settings_on_rail &&
               app_settings_rail_index(app->settings_cat) == 0;
    }
    return false;
}

bool app_store_available(void)
{
    const char *env = getenv("TAIJI_STORE_BIN");
    char path[PATH_MAX];
    if (env && *env) snprintf(path, sizeof(path), "%s", env);
    else snprintf(path, sizeof(path), "/usr/bin/taiji-store");
    /* 导航只检测商店主程序；helper/后端问题由商店内部提示，避免入口被误隐藏。 */
    return access(path, X_OK) == 0;
}

void app_launch_store(App *app)
{
    if (!app_store_available()) {
        ui_toast(app, "商店未安装");
        return;
    }
    const char *env = getenv("TAIJI_STORE_BIN");
    char path[PATH_MAX];
    if (env && *env) snprintf(path, sizeof(path), "%s", env);
    else snprintf(path, sizeof(path), "/usr/bin/taiji-store");
    const char *argv[] = {path, NULL};
    spawn_detached(argv);
}

static int nav_index_for_screen(App *app)
{
    Screen want = app->screen == SCREEN_EDITOR ? SCREEN_FILES : app->screen;
    for (int i = 0; i < app->ui.nav_count; i++) {
        lv_obj_t *item = app->ui.nav_items[i];
        intptr_t v = item ? (intptr_t)lv_obj_get_user_data(item) : 0;
        if (v > 0 && (Screen)(v - 1) == want) return i;
    }
    return 0;
}

static Screen nav_screen_at(App *app, int index)
{
    if (index < 0 || index >= app->ui.nav_count) return SCREEN_HOME;
    lv_obj_t *item = app->ui.nav_items[index];
    intptr_t v = item ? (intptr_t)lv_obj_get_user_data(item) : 0;
    if (v > 0) return (Screen)(v - 1);
    if (index < 8) return app->ui.nav_screens[index];
    return SCREEN_HOME;
}

static void app_input_nav(App *app, InputAction action)
{
    int n = app->ui.nav_count > 0 ? app->ui.nav_count : 4;
    bool picking = app->files_pick != FILES_PICK_NONE;
    int only = nav_index_for_screen(app);
    if (picking) {
        app->nav_focus = only;
        ui_update_nav_focus(app);
    }
    if (action == IN_LEFT) {
        if (picking) return;
        if (app->nav_focus <= 0) {
            /* 导航栏和状态栏同一层级：继续向左进入状态栏。 */
            if (app->ui.status_item_count > 0) {
                app->nav_on_bar = false;
                app->status_on_bar = true;
                app->status_focus = app->ui.status_item_count - 1;
                ui_update_nav_focus(app);
                ui_update_status_focus(app);
                audio_play(SND_NAV);
                return;
            }
        }
        app->nav_focus = (app->nav_focus - 1 + n) % n;
        ui_update_nav_focus(app);
        audio_play(SND_NAV);
    } else if (action == IN_RIGHT) {
        if (picking) return;
        if (app->nav_focus + 1 >= n) {
            /* 最后一个导航项向右进入状态栏。 */
            if (app->ui.status_item_count > 0) {
                app->nav_on_bar = false;
                app->status_on_bar = true;
                app->status_focus = 0;
                ui_update_nav_focus(app);
                ui_update_status_focus(app);
                audio_play(SND_NAV);
                return;
            }
        }
        app->nav_focus = (app->nav_focus + 1) % n;
        ui_update_nav_focus(app);
        audio_play(SND_NAV);
    } else if (action == IN_UP) {
        /* 上键不再进入状态栏：状态栏只从最右侧导航项按右进入。 */
        app->nav_on_bar = false;
        app->status_on_bar = false;
        ui_update_nav_focus(app);
        audio_play(SND_BACK);
    } else if (action == IN_DOWN || action == IN_CANCEL ||
               action == IN_B) {
        app->nav_on_bar = false;
        ui_update_nav_focus(app);
        audio_play(SND_BACK);
    } else if (action == IN_CONFIRM || action == IN_A) {
        app->nav_on_bar = false;
        if (app->screen == SCREEN_EDITOR && app->editor.active &&
            app->editor.dirty) {
            ui_update_nav_focus(app);
            app_open_confirm(app, CONFIRM_EDITOR_DISCARD, 0,
                             "放弃修改？", "有未保存的修改，离开将丢失。");
            return;
        }
        if (picking) {
            ui_update_nav_focus(app);
            audio_play(SND_BACK);
            return;
        }
        Screen target = nav_screen_at(app, app->nav_focus);
        app_goto_screen(app, target);
        audio_play(SND_NAV_STRONG);
    }
}

static void app_input_status(App *app, InputAction action)
{
    int n = app->ui.status_item_count;
    if (n <= 0) { app->status_on_bar = false; return; }
    if (action == IN_LEFT) {
        if (app->status_focus <= 0 && app->ui.nav_count > 0) {
            /* 从状态栏最左继续向左回到导航栏最后一个入口。 */
            app->status_on_bar = false;
            app->nav_on_bar = true;
            app->nav_focus = app->ui.nav_count - 1;
            ui_update_status_focus(app);
            ui_update_nav_focus(app);
            audio_play(SND_NAV);
            return;
        }
        if (app->status_focus > 0) app->status_focus--;
        ui_update_status_focus(app);
        audio_play(SND_NAV);
    } else if (action == IN_RIGHT) {
        if (app->status_focus + 1 >= n && app->ui.nav_count > 0) {
            /* 从状态栏最右继续向右回到导航栏第一个入口。 */
            app->status_on_bar = false;
            app->nav_on_bar = true;
            app->nav_focus = 0;
            ui_update_status_focus(app);
            ui_update_nav_focus(app);
            audio_play(SND_NAV);
            return;
        }
        if (app->status_focus + 1 < n) app->status_focus++;
        ui_update_status_focus(app);
        audio_play(SND_NAV);
    } else if (action == IN_CONFIRM || action == IN_A) {
        int kind = app->ui.status_kinds[app->status_focus];
        app->status_on_bar = false;
        ui_update_status_focus(app);
        app_open_control_center_view(app, kind);
        audio_play(SND_ENTER);
    } else if (action == IN_DOWN) {
        app->status_on_bar = false;
        app->nav_on_bar = true;
        app->nav_focus = nav_index_for_screen(app);
        ui_update_status_focus(app);
        ui_update_nav_focus(app);
        audio_play(SND_BACK);
    } else if (action == IN_CANCEL || action == IN_B) {
        app->status_on_bar = false;
        app->nav_on_bar = false;
        ui_update_status_focus(app);
        ui_update_nav_focus(app);
        audio_play(SND_BACK);
    }
}

/* Volume keys must always win over a modal that is still fading out.  Without
 * this the OSD is silently swallowed when the previous card is closing. */
static void app_force_close_modal(App *app)
{
    app->modal = MODAL_NONE;
    app->modal_closing = false;
    app->control_closing = false;
    app->control_osd = false;
    ui_set_input_active(false);
    ui_close_modal_visual(app);
}

static void app_show_volume_card(App *app)
{
    if (app->ui_suspended) return;
    if (app->modal_closing || app->control_closing)
        app_force_close_modal(app);
    if (app->modal == MODAL_NONE || app->modal == MODAL_CONTROL) {
        if (app->modal != MODAL_CONTROL || app->control_view != STATUS_VOLUME)
            app_open_control_center_view(app, STATUS_VOLUME);
        app->control_osd = true;
        app->control_osd_until = lv_tick_get() + 2000;
    }
}

static void app_trace(const char *fmt, ...)
{
    const char *path = getenv("ARMDECK_OSK_LOG");
    if (!path || !*path) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "input ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void app_input(App *app, InputAction action, const char *text)
{
    if (action == IN_NONE) return;
    app_trace("action=%d modal=%d text=%s", (int)action, app->modal,
              text ? text : "");
    if (action == IN_PAD_CONFIRM) action = IN_CONFIRM;
    else if (action == IN_PAD_CANCEL) action = IN_CANCEL;
    if (action == IN_VOLUME_UP || action == IN_VOLUME_DOWN) {
        int delta = action == IN_VOLUME_UP ? 5 : -5;
        if (app->ui_suspended) {
            app_control_volume(app, delta);
            system_volume_poll(&app->sysvol);
            volume_osd_send(app->sysvol.percent, app->sysvol.muted);
        } else {
            app_show_volume_card(app);
            app_control_volume(app, delta);
        }
        return;
    }
    if (action == IN_VOLUME_MUTE) {
        if (app->ui_suspended) {
            system_volume_toggle_mute(&app->sysvol);
            volume_osd_send(app->sysvol.percent, app->sysvol.muted);
            return;
        }
        app_show_volume_card(app);
        if (system_volume_toggle_mute(&app->sysvol)) {
            if (app->ui.modal && app->control_view == STATUS_VOLUME)
                ui_update_volume_modal(app);
            ui_update_status(app);
            app_show_volume_card(app);
        } else {
            ui_toast(app, "系统音量不可用（需要 PipeWire / wpctl）");
        }
        return;
    }
    if (app->modal_closing) return;
    if (app->control_closing) return;
    if (app->ui_suspended) {
        if (action == IN_QUIT) app->backend.running = false;
        return;
    }
    if (action == IN_SCROLL_UP) action = IN_UP;
    else if (action == IN_SCROLL_DOWN) action = IN_DOWN;
    if (action == IN_QUIT) {
        app_open_confirm(app, CONFIRM_QUIT, 0, "退出太极？", "当前设置已经自动保存。");
        return;
    }
    if (action == IN_FULLSCREEN) {
        const char *kiosk = getenv("TAIJI_KIOSK");
        if (kiosk && *kiosk == '1') {
            ui_toast(app, "Kiosk 会话模式：请用 Ctrl+Q 退出");
            return;
        }
        backend_toggle_fullscreen(&app->backend);
        if (app->modal != MODAL_NONE) ui_build_modal(app);
        return;
    }
    if (action == IN_SCAN && app->modal == MODAL_NONE && !app->oobe) {
        app_rescan(app);
        return;
    }
    if (app->modal != MODAL_NONE) { modal_input(app, action, text); return; }
    if (app->oobe && app->files_pick == FILES_PICK_NONE) {
        oobe_input(app, action);
        return;
    }
    if (app->screen == SCREEN_EDITOR &&
        (action == IN_LB || action == IN_RB)) {
        editor_input(app, action, text);
        return;
    }
    /* Shoulder buttons switch the top-level tabs on every screen, including
     * the embedded file manager and the overview. */
    if (action == IN_LB || action == IN_RB) {
        int dir = action == IN_LB ? -1 : 1;
        taiji_debug_log("shoulder %s", dir < 0 ? "L" : "R");
        switch_tab(app, dir);
        return;
    }
    if (app->status_on_bar) { app_input_status(app, action); return; }
    if (app->nav_on_bar) { app_input_nav(app, action); return; }
    if (app->screen == SCREEN_OVERVIEW) { input_overview(app, action); return; }
    if (action == IN_UP && app_at_top_edge(app) && app->ui.nav_count > 0) {
        app->nav_on_bar = true;
        app->nav_focus = nav_index_for_screen(app);
        app->status_on_bar = false;
        ui_update_status_focus(app);
        ui_update_nav_focus(app);
        audio_play(SND_NAV);
        return;
    }
    if (app->screen == SCREEN_STORE) {
        input_store(app, action, text);
        return;
    }
    if (app->screen == SCREEN_FILES) { input_files(app, action); return; }
    if (app->screen == SCREEN_EDITOR) { editor_input(app, action, text); return; }
    if (app->screen == SCREEN_HOME) input_home(app, action);
    else if (app->screen == SCREEN_GAMES || app->screen == SCREEN_FAVORITES || app->screen == SCREEN_RECENT) input_games(app, action);
    else if (app->screen == SCREEN_APPS) input_apps(app, action);
    else input_settings(app, action);
}

/* ── App lifecycle ────────────────────────────────────────────────────────── */

static void app_test_tokens(App *app, const char *tokens)
{
    if (!tokens || !*tokens) return;
    {
        char *copy = xstrdup(tokens);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "down") == 0) app_input(app, IN_DOWN, NULL);
            else if (strcmp(tok, "up") == 0) app_input(app, IN_UP, NULL);
            else if (strcmp(tok, "left") == 0) app_input(app, IN_LEFT, NULL);
            else if (strcmp(tok, "right") == 0) app_input(app, IN_RIGHT, NULL);
            else if (strcmp(tok, "confirm") == 0) app_input(app, IN_CONFIRM, NULL);
            else if (strcmp(tok, "pada") == 0) app_input(app, IN_PAD_CONFIRM, NULL);
            else if (strcmp(tok, "padb") == 0) app_input(app, IN_PAD_CANCEL, NULL);
            else if (strcmp(tok, "back") == 0) app_input(app, IN_CANCEL, NULL);
            else if (strcmp(tok, "x") == 0) app_input(app, IN_X, NULL);
            else if (strcmp(tok, "y") == 0) app_input(app, IN_Y, NULL);
            else if (strcmp(tok, "lb") == 0) app_input(app, IN_LB, NULL);
            else if (strcmp(tok, "rb") == 0) app_input(app, IN_RB, NULL);
            else if (strcmp(tok, "lt") == 0) app_input(app, IN_LT, NULL);
            else if (strcmp(tok, "rt") == 0) app_input(app, IN_RT, NULL);
            else if (strcmp(tok, "menu") == 0) app_input(app, IN_START, NULL);
            else if (strcmp(tok, "select") == 0) app_input(app, IN_SELECT, NULL);
            else if (strcmp(tok, "help") == 0) { app->modal = MODAL_HELP; ui_build_modal(app); }
            else if (strcmp(tok, "detail") == 0) { app->modal = MODAL_DETAIL; ui_build_modal(app); }
            else if (strcmp(tok, "control") == 0) app_open_control_center(app);
            else if (strcmp(tok, "power") == 0) app_open_power_menu(app);
            else if (strcmp(tok, "gamepad") == 0) { app->modal = MODAL_GAMEPAD; ui_build_modal(app); }
            else if (strcmp(tok, "overview") == 0) app_open_overview(app);
            else if (strcmp(tok, "osdtest") == 0) volume_osd_send(42, false);
            else if (strcmp(tok, "battery") == 0) app_open_control_center_view(app, STATUS_BATTERY);
            else if (strcmp(tok, "padcontrol") == 0) app_open_control_center_view(app, STATUS_PAD);
            else if (strcmp(tok, "volume") == 0) app_open_control_center_view(app, STATUS_VOLUME);
            else if (strcmp(tok, "volup") == 0) app_input(app, IN_VOLUME_UP, NULL);
            else if (strcmp(tok, "voldown") == 0) app_input(app, IN_VOLUME_DOWN, NULL);
            else if (strcmp(tok, "volmute") == 0) app_input(app, IN_VOLUME_MUTE, NULL);
            else if (strcmp(tok, "search") == 0) { app->modal = MODAL_SEARCH; app->search_buf[0] = '\0'; app->search_focus = 0; ui_set_input_active(true); ui_build_modal(app); }
            else if (strcmp(tok, "text") == 0) app_open_text(app, TEXT_ROOT, 0, "添加游戏目录", "", "输入 ROM 根目录路径");
            else if (strcmp(tok, "quitconfirm") == 0) app_open_confirm(app, CONFIRM_QUIT, 0, "退出太极？", "当前设置已经自动保存。");
            else if (strcmp(tok, "mapping") == 0) { app->modal = MODAL_MAPPING; app->mapping_step = 0; backend_set_capture(true); ui_build_modal(app); }
            else if (strncmp(tok, "type:", 5) == 0) app_input(app, IN_TEXT, tok + 5);
            else if (strncmp(tok, "edit:", 5) == 0) editor_open(app, tok + 5);
            else if (strcmp(tok, "connect") == 0) {
                app_goto_screen(app, SCREEN_FILES);
                app->files_panel = FILES_PANEL_CONNECT;
                app->smb_ui.focus = 0;
                app->smb_ui.view = 0;
                app_rebuild(app);
            } else if (strcmp(tok, "manage") == 0) {
                app_goto_screen(app, SCREEN_FILES);
                app->files_panel = FILES_PANEL_MANAGE;
                smb_local_refresh(app);
                smb_service_query(app);
                app->smb_ui.focus = 0;
                app_rebuild(app);
            }
            else if (strcmp(tok, "settings") == 0) app_goto_screen(app, SCREEN_SETTINGS);
            else if (strcmp(tok, "favorites") == 0) app_goto_screen(app, SCREEN_FAVORITES);
        }
        free(copy);
    }
}

void app_run(App *app)
{
    if (!backend_init(&app->backend, 1280, 720, on_backend_input, app)) {
        /* A requested renderer can be listed by SDL but still fail to create a
         * context on some drivers (e.g. a broken GLES stack). Retry once with
         * SDL's automatic driver selection instead of leaving the session dead. */
        const char *requested = getenv("SDL_RENDER_DRIVER");
        if (requested && *requested) {
            fprintf(stderr, "[taiji] renderer '%s' failed to start, retrying with auto\n", requested);
            unsetenv("SDL_RENDER_DRIVER");
            if (!backend_init(&app->backend, 1280, 720, on_backend_input, app)) {
                fprintf(stderr, "failed to initialize SDL backend\n");
                return;
            }
        } else {
            fprintf(stderr, "failed to initialize SDL backend\n");
            return;
        }
    }

    system_status_poll(&app->sys);
    system_volume_poll(&app->sysvol);
    perf_poll(&app->perf);
    gpu_poll(&app->gpu);

    /* Kiosk / fullscreen session startup is kept in the application layer so the
     * SDL display backend itself stays unchanged. */
    const char *kiosk_env = getenv("TAIJI_KIOSK");
    const char *fs_env = getenv("TAIJI_FULLSCREEN");
    if ((kiosk_env && *kiosk_env == '1') || (fs_env && *fs_env == '1')) {
        SDL_SetWindowFullscreen(app->backend.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        app->backend.fullscreen = true;
        if (kiosk_env && *kiosk_env == '1') {
            SDL_SetWindowBordered(app->backend.window, SDL_FALSE);
            SDL_SetWindowAlwaysOnTop(app->backend.window, SDL_TRUE);
        }
    } else {
        SDL_Rect usable;
        const char *allow_big = getenv("TAIJI_TEST_ALLOW_OVERSIZE");
        if ((!allow_big || *allow_big != '1') && SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(app->backend.window, &ww, &wh);
            if (ww > usable.w || wh > usable.h) {
                if (ww > usable.w) ww = usable.w;
                if (wh > usable.h) wh = usable.h;
                SDL_SetWindowSize(app->backend.window, ww, wh);
                SDL_SetWindowPosition(app->backend.window, SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED);
            }
        }
    }
    SDL_ShowCursor(SDL_ENABLE);
    backend_refresh_size(&app->backend);
    app->backend.resized = false;

    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(app->backend.renderer, &ri) == 0) {
        snprintf(app->renderer_name, sizeof(app->renderer_name), "%s", ri.name ? ri.name : "unknown");
        snprintf(app->renderer_info, sizeof(app->renderer_info), "%s | %s%s%s",
                 ri.name ? ri.name : "unknown",
                 (ri.flags & SDL_RENDERER_ACCELERATED) ? "accelerated" : "software",
                 (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? " | vsync" : "",
                 (ri.flags & SDL_RENDERER_TARGETTEXTURE) ? " | render-to-texture" : "");
        fprintf(stderr, "[taiji] SDL renderer: %s\n", app->renderer_info);
    } else {
        snprintf(app->renderer_name, sizeof(app->renderer_name), "unknown");
    }

    theme_init();
    app->active_theme = app_theme_preset(app);
    theme_set_preset(app->active_theme);
    audio_init();
    audio_set_enabled(app->config.sound);
    audio_set_volume(app->config.volume);
    app_apply_system_scale(app);
    app_apply_gamepad_config(app);
    app_update_pad_labels(app);

    if (!app->oobe) {
        if (!library_load_cache(&app->library)) {
            ui_splash(app, "正在扫描游戏库...");
            library_scan(&app->library, &app->config, scan_progress, app);
        } else if (app->config.scan_on_start && app->config.root_count > 0) {
            ui_splash(app, "正在更新游戏库...");
            library_scan(&app->library, &app->config, scan_progress, app);
        }
    }

    Collection cols[128];
    int n = app_collections(app, cols, 128);
    for (int i = 0; i < n; i++) {
        if (strcmp(cols[i].id, app->collection_id) == 0) { app->home_focus = i; break; }
    }
    app_rebuild(app);
    const char *start_screen = app->oobe ? NULL : getenv("TAIJI_START_SCREEN");
    if (start_screen) {
        if (strcmp(start_screen, "settings") == 0) app_goto_screen(app, SCREEN_SETTINGS);
        else if (strcmp(start_screen, "favorites") == 0) app_goto_screen(app, SCREEN_FAVORITES);
        else if (strcmp(start_screen, "recent") == 0) app_goto_screen(app, SCREEN_RECENT);
        else if (strcmp(start_screen, "apps") == 0) app_goto_screen(app, SCREEN_APPS);
        else if (strcmp(start_screen, "games") == 0) {
            const char *collection = getenv("TAIJI_START_COLLECTION");
            app_enter_collection(app, collection && *collection ? collection : "all");
            if (collection && strcmp(collection, "all") != 0) {
                /* focus the first game of the collection */
            }
        }
    }
    app_test_tokens(app, getenv("TAIJI_TEST_ACTION"));
    if (app->exit_code == 0) app->backend.running = true;
    const char *shot_path = getenv("TAIJI_SCREENSHOT");
    uint32_t shot_start = lv_tick_get();
    const char *resize_env = getenv("TAIJI_TEST_RESIZE");
    int resize_w = 0, resize_h = 0, resize_done = 0;
    bool status_click_done = false;
    bool volume_click_done = false;
    if (resize_env && sscanf(resize_env, "%dx%d", &resize_w, &resize_h) != 2) {
        resize_w = resize_h = 0;
    }

    uint32_t last_runtime_poll = 0;
    uint32_t last_status_poll = 0;
    uint32_t last_volume_poll = 0;
    uint32_t last_power_poll = 0;
    uint32_t last_desktop_watch = 0;
    desktop_watch_init();
    app->desktop_recheck_at = lv_tick_get() + 3000;
    bluetooth_scan_async(false);
    while (app->backend.running) {
        backend_poll(&app->backend);
        external_osk_poll(app);
        {
            static long cmd_off;
            const char *cmdfile = getenv("TAIJI_TEST_CMDS");
            if (cmdfile && *cmdfile) {
                int cfd = open(cmdfile, O_RDONLY | O_CLOEXEC);
                if (cfd >= 0) {
                    off_t sz = lseek(cfd, 0, SEEK_END);
                    if (sz > cmd_off) {
                        char cb[512];
                        size_t want = (size_t)(sz - cmd_off);
                        if (want > sizeof(cb) - 1) want = sizeof(cb) - 1;
                        lseek(cfd, (off_t)cmd_off, SEEK_SET);
                        ssize_t got = read(cfd, cb, want);
                        if (got > 0) {
                            cb[got] = 0;
                            char *save2 = NULL;
                            for (char *ln = strtok_r(cb, "\n", &save2); ln;
                                 ln = strtok_r(NULL, "\n", &save2))
                                app_test_tokens(app, ln);
                            cmd_off += got;
                        }
                    }
                    close(cfd);
                }
            }
        }
        app_scan_poll(app);
        app_desktop_scan_poll(app);
        sig_atomic_t volume_sig = g_volume_signal;
        if (volume_sig) {
            g_volume_signal = 0;
            if (volume_sig == 1) app_input(app, IN_VOLUME_UP, NULL);
            else if (volume_sig == 2) app_input(app, IN_VOLUME_DOWN, NULL);
            else if (volume_sig == 3) app_input(app, IN_VOLUME_MUTE, NULL);
        }
        if (lv_tick_get() - last_runtime_poll > 150) {
            last_runtime_poll = lv_tick_get();
            if (app->oobe) ui_oobe_tick(app);
            app_refresh_runtime(app);
            app_files_poll(app);
            if (app->screen == SCREEN_STORE) {
                app_store_poll(app);
                if (app->store_task_running) app_rebuild(app);
            }
        }
        static uint32_t last_pad_poll = 0;
        if (lv_tick_get() - last_pad_poll > 500) {
            last_pad_poll = lv_tick_get();
            app_update_pad_labels(app);
        }
        {
            static uint32_t last_wifi_poll = 0;
            if (lv_tick_get() - last_wifi_poll > 100) {
                last_wifi_poll = lv_tick_get();
                bool changed = false;
                WifiState ws;
                if (network_wifi_scan_take(&ws)) {
                    app->wifi = ws;
                    changed = true;
                }
                BtState bs;
                if (bluetooth_scan_take(&bs)) {
                    if (memcmp(&app->bt, &bs, sizeof(bs)) != 0) {
                        app->bt = bs;
                        changed = true;
                    }
                }
                char bt_msg[256];
                if (bluetooth_action_take(bt_msg, sizeof(bt_msg))) {
                    if (bt_msg[0]) ui_toast(app, bt_msg);
                    bluetooth_scan_async(true);
                    changed = true;
                }
                if (changed) {
                    int count = app->control_radio == 1 ?
                        app->bt.count : app->wifi.count;
                    if (app->control_focus >= count)
                        app->control_focus = count > 0 ? count - 1 : 0;
                    if (app->modal == MODAL_CONTROL && !app->modal_closing &&
                        app->control_view == STATUS_WIFI)
                        ui_refresh_control_modal(app);
                    ui_update_status(app);
                }
                if (app->modal == MODAL_CONTROL && !app->modal_closing &&
                    app->control_view == STATUS_WIFI &&
                    app->control_radio == 1) {
                    static uint32_t last_bt_idle = 0;
                    if (lv_tick_get() - last_bt_idle > 5000) {
                        last_bt_idle = lv_tick_get();
                        bluetooth_scan_async(false);
                    }
                }
            }
        }
        if (app->modal == MODAL_GAMEPAD) {
            static uint32_t last_gamepad_poll = 0;
            if (lv_tick_get() - last_gamepad_poll > 50) {
                last_gamepad_poll = lv_tick_get();
                ui_gamepad_poll(app);
            }
        }
        if (lv_tick_get() - last_power_poll > 200) {
            last_power_poll = lv_tick_get();
            char power_path[512];
            if (!app->ui_suspended &&
                power_request_pending(power_path, sizeof(power_path))) {
                unlink(power_path);
                if (app->modal != MODAL_POWER) app_open_power_menu(app);
            }
        }
        {
            static uint32_t last_session_poll = 0;
            if (lv_tick_get() - last_session_poll > 150) {
                last_session_poll = lv_tick_get();
                char req[512];
                if (app->oobe) {
                    /* 初始设置期间忽略概览/返回请求，避免盖住 OOBE 或关掉日期弹窗。 */
                    session_request_clear("overview_request");
                    session_request_clear("return_request");
                } else {
                    if (session_request_pending("return_request", req, sizeof(req))) {
                        session_request_clear("return_request");
                        if (app->ui_suspended) app_set_ui_suspended(app, false);
                        if (app->screen == SCREEN_OVERVIEW) app_close_overview(app);
                    }
                    if (session_request_pending("overview_request", req, sizeof(req))) {
                        session_request_clear("overview_request");
                        app_open_overview(app);
                    }
                }
                if (session_request_pending("brightness_request", req, sizeof(req))) {
                    size_t blen = 0;
                    char *bdata = read_file(req, &blen);
                    session_request_clear("brightness_request");
                    int delta = bdata ? atoi(bdata) : 0;
                    free(bdata);
                    if (delta) app_brightness_step(app, delta);
                }
            }
        }
        if (lv_tick_get() - last_volume_poll > 2000) {
            last_volume_poll = lv_tick_get();
            system_volume_poll(&app->sysvol);
            ui_update_status(app);
            if (app->modal == MODAL_CONTROL && app->control_view == STATUS_VOLUME)
                ui_update_volume_modal(app);
        }
        if (app->control_osd && app->modal == MODAL_CONTROL &&
            app->control_view == STATUS_VOLUME && !app->modal_closing &&
            (int32_t)(lv_tick_get() - app->control_osd_until) >= 0) {
            app->control_osd = false;
            app_close_modal(app);
        }
        if (!app->ui_suspended &&
            lv_tick_get() - last_desktop_watch > 150) {
            last_desktop_watch = lv_tick_get();
            if (desktop_watch_poll()) {
                app_desktop_scan_start(app, false);
                app->desktop_recheck_at = lv_tick_get() + 3000;
            }
        }
        if (app->desktop_recheck_at &&
            (int32_t)(lv_tick_get() - app->desktop_recheck_at) >= 0) {
            app->desktop_recheck_at = 0;
            app_desktop_scan_start(app, false);
        }
        if (app->apps_refresh_pending && app->screen == SCREEN_APPS &&
            app->modal == MODAL_NONE && !app->modal_closing) {
            app->apps_refresh_pending = false;
            app->app_badge = false;
            app_filter_apps(app);
            if (app->app_focus >= app->app_filter_count)
                app->app_focus = app->app_filter_count > 0 ?
                                app->app_filter_count - 1 : 0;
            app_rebuild(app);
            ui_update_app_badge(app);
        }
        if (app->config.theme_auto &&
            lv_tick_get() - app->theme_auto_check > 15000) {
            app->theme_auto_check = lv_tick_get();
            if (app->modal == MODAL_NONE && !app->ui_suspended) {
                int preset = app_theme_preset(app);
                if (preset != app->active_theme) {
                    app->active_theme = preset;
                    theme_set_preset(preset);
                    app_rebuild(app);
                }
            }
        }
        if (lv_tick_get() - last_status_poll > 10000) {
            last_status_poll = lv_tick_get();
            system_status_poll(&app->sys);
            perf_poll(&app->perf);
            gpu_poll(&app->gpu);
            ui_update_status(app);
        }
        if (app->backend.resized) {
            app->backend.resized = false;
            app_rebuild(app);
        }

        if (app->modal == MODAL_MAPPING) {
            int captured = backend_take_captured_button();
            if (captured >= 0) {
                if (app->mapping_step >= 0 && app->mapping_step < 4) {
                    app->mapping_values[app->mapping_step] = captured;
                    app->mapping_step++;
                }
                if (app->mapping_step >= 4) {
                    app->config.custom_mapping = true;
                    app->config.map_confirm = app->mapping_values[0];
                    app->config.map_cancel = app->mapping_values[1];
                    app->config.map_menu1 = app->mapping_values[2];
                    app->config.map_menu2 = app->mapping_values[3];
                    app_save_settings(app);
                    backend_set_capture(false);
                    app_close_modal(app);
                    app_rebuild(app);
                    ui_toast(app, "自定义按键映射已保存");
                } else {
                    ui_build_modal(app);
                }
            }
        }

        if (resize_w > 0 && !resize_done && lv_tick_get() - shot_start > 1500) {
            SDL_SetWindowSize(app->backend.window, resize_w, resize_h);
            resize_done = 1;
        }
        const char *status_click = getenv("TAIJI_TEST_STATUS_CLICK");
        if (status_click && *status_click && !status_click_done &&
            lv_tick_get() - shot_start > 900) {
            int which = atoi(status_click);
            if (which < 1) which = 1;
            if (which > app->ui.status_item_count) which = app->ui.status_item_count;
            int cx = app->layout.top_x + app->layout.top_w - app->layout.status_w / 2;
            int cy = app->layout.top_y + app->layout.top_h / 2;
            if (which > 0 && app->ui.status_items[which - 1]) {
                lv_area_t a;
                lv_obj_get_coords(app->ui.status_items[which - 1], &a);
                cx = (a.x1 + a.x2) / 2;
                cy = (a.y1 + a.y2) / 2;
            }
            int wx = cx, wy = cy;
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_MOUSEMOTION;
            ev.motion.x = wx; ev.motion.y = wy;
            SDL_PushEvent(&ev);
            SDL_zero(ev);
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.x = wx; ev.button.y = wy; ev.button.button = SDL_BUTTON_LEFT;
            ev.button.state = SDL_PRESSED; ev.button.clicks = 1;
            SDL_PushEvent(&ev);
            SDL_zero(ev);
            ev.type = SDL_MOUSEBUTTONUP;
            ev.button.x = wx; ev.button.y = wy; ev.button.button = SDL_BUTTON_LEFT;
            ev.button.state = SDL_RELEASED; ev.button.clicks = 1;
            SDL_PushEvent(&ev);
            status_click_done = true;
        }

        const char *volume_click = getenv("TAIJI_TEST_VOLUME_CLICK");
        if (volume_click && *volume_click && !volume_click_done &&
            app->modal == MODAL_CONTROL && app->control_view == STATUS_VOLUME &&
            app->ui.control_volume_bar && app->ui.control_volume_slider &&
            lv_tick_get() - shot_start > 900) {
            int pct = atoi(volume_click);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            lv_area_t fill_area, knob_area;
            lv_obj_get_coords(app->ui.control_volume_bar, &fill_area);
            lv_obj_get_coords(app->ui.control_volume_slider, &knob_area);
            int origin = fill_area.x1 - app->ui.control_volume_track_x;
            int cx = origin + app->ui.control_volume_track_x +
                     app->ui.control_volume_track_w * pct / 100;
            int cy = (knob_area.y1 + knob_area.y2) / 2;
            int wx = cx, wy = cy;
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_MOUSEMOTION;
            ev.motion.x = wx; ev.motion.y = wy;
            SDL_PushEvent(&ev);
            SDL_zero(ev);
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.x = wx; ev.button.y = wy; ev.button.button = SDL_BUTTON_LEFT;
            ev.button.state = SDL_PRESSED; ev.button.clicks = 1;
            SDL_PushEvent(&ev);
            SDL_zero(ev);
            ev.type = SDL_MOUSEBUTTONUP;
            ev.button.x = wx; ev.button.y = wy; ev.button.button = SDL_BUTTON_LEFT;
            ev.button.state = SDL_RELEASED; ev.button.clicks = 1;
            SDL_PushEvent(&ev);
            volume_click_done = true;
        }

        bool was_external = app->launcher.external;
        bool was_game = app->launcher.game_session;
        int64_t elapsed = 0;
        if (launcher_poll(&app->launcher, &elapsed) == 1) {
            if (was_external && was_game) {
                app_set_ui_suspended(app, false);
                Game *gg = library_find_game(&app->library, app->active_game_id);
                if (gg && elapsed >= 2) {
                    library_note_play(&app->library, gg, elapsed);
                    char *fmt = format_playtime(elapsed);
                    char msg[512];
                    snprintf(msg, sizeof(msg), "%s / 本次游玩 %s", gg->name, fmt);
                    free(fmt);
                    ui_toast(app, msg);
                } else {
                    ui_toast(app, "游戏已退出");
                }
                app->active_game_id[0] = '\0';
                apps_runtime_read(&app->runtime);
                ui_update(app);
            } else if (was_external) {
                app_set_ui_suspended(app, false);
                ui_toast(app, "应用已退出，已返回太极");
                app->active_app_id[0] = '\0';
                app->external_paused = false;
                apps_runtime_read(&app->runtime);
                ui_update(app);
            } else {
                SDL_RestoreWindow(app->backend.window);
                SDL_RaiseWindow(app->backend.window);
                Game *g = library_find_game(&app->library, app->active_game_id);
                if (g && elapsed >= 2) {
                    library_note_play(&app->library, g, elapsed);
                    char *fmt = format_playtime(elapsed);
                    char msg[512];
                    snprintf(msg, sizeof(msg), "%s / 本次游玩 %s", g->name, fmt);
                    free(fmt);
                    ui_toast(app, msg);
                    ui_update(app);
                } else {
                    ui_toast(app, "启动器已退出，请检查模拟器命令（设置 至 模拟器）");
                }
                app->active_game_id[0] = '\0';
            }
        }

        if (!app->ui_suspended) {
            lv_timer_handler();
            lv_refr_now(NULL);
            ui_tick(app);
            backend_present(&app->backend);
        }
        const char *delay_env = getenv("TAIJI_SCREENSHOT_DELAY");
        uint32_t delay = delay_env && *delay_env ? (uint32_t)atoi(delay_env) : 1200u;
        if (shot_path && *shot_path && lv_tick_get() - shot_start > delay) {
            backend_save_bmp(&app->backend, shot_path);
            app->backend.running = false;
        }
    }

    if (app->launcher.active && app->launcher.external && app->launcher.pid > 0) {
        kill(-app->launcher.pid, SIGTERM);
    }
    if (app->files_op_running && app->files_op_pid > 0) {
        kill(-app->files_op_pid, SIGTERM);
    }
    app_save_settings(app);
    library_save_cache(&app->library);
    apps_save(&app->apps);
    app_store_free(app);
    external_osk_close(app);
    audio_shutdown();
    backend_shutdown(&app->backend);
}

void app_free(App *app)
{
    if (app->scan_running) {
        pthread_join(app->scan_thread, NULL);
        app->scan_running = false;
    }
    if (app->desktop_scan_running) {
        pthread_join(app->desktop_scan_thread, NULL);
        app->desktop_scan_running = false;
    }
    library_free(&app->scan_result);
    library_free(&app->library);
    apps_free(&app->apps);
    config_free(&app->config);
    launcher_free(&app->launcher);
    if (app->ui.root) app->ui.root = NULL;
}
