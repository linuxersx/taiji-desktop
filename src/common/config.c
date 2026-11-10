// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *config_file_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/config.ini", taiji_config_dir());
    return buf;
}

void config_init(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->swap_ab = false;
    c->swap_xy = false;
    c->swap_ps = false;
    c->custom_mapping = false;
    c->map_confirm = -1;
    c->map_cancel = -1;
    c->map_menu1 = -1;
    c->map_menu2 = -1;
    c->language = 0;
    c->theme = 0;
    c->theme_auto = false;
    c->auto_dark_min = -1;
    c->auto_light_min = -1;
    c->sound = true;
    c->volume = 60;
    c->ui_scale = 100;
    c->ui_scale_auto = true;
    c->refresh_hz = 0;
    c->render_scale = 100;
    snprintf(c->renderer, sizeof(c->renderer), "opengles2");
    c->grid_columns = 0;
    c->scan_on_start = true;
    c->show_cover_panel = true;
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
    c->low_power = true;
#else
    c->low_power = false;
#endif
    c->app_mode = 0;
    c->max_paused_apps = 1;
    c->store_pkg_type = 0;
    c->store_pacman_source = 1;
    c->store_flatpak_source = 1;
    const char *low = getenv("TAIJI_LOW_POWER");
    if (low && *low) c->low_power = (low[0] == '1');
    c->last_platform = xstrdup("all");
    c->last_game = xstrdup("");
}

static void free_commands(Config *c)
{
    for (int i = 0; i < c->command_count; i++) {
        free(c->commands[i].platform_id);
        free(c->commands[i].command);
    }
    free(c->commands);
    c->commands = NULL;
    c->command_count = 0;
}

void config_free(Config *c)
{
    for (int i = 0; i < c->root_count; i++) free(c->roots[i]);
    free(c->roots);
    free_commands(c);
    free(c->last_platform);
    free(c->last_game);
    memset(c, 0, sizeof(*c));
}

void config_add_root(Config *c, const char *path)
{
    if (!path || !*path) return;
    char *abs = path_absolute(path);
    size_t len = strlen(abs);
    while (len > 1 && abs[len - 1] == '/') abs[--len] = '\0';
    for (int i = 0; i < c->root_count; i++) {
        if (strcmp(c->roots[i], abs) == 0) { free(abs); return; }
    }
    c->roots = xrealloc(c->roots, (size_t)(c->root_count + 1) * sizeof(char *));
    c->roots[c->root_count++] = abs;
}

void config_remove_root(Config *c, int index)
{
    if (index < 0 || index >= c->root_count) return;
    free(c->roots[index]);
    for (int i = index; i < c->root_count - 1; i++) c->roots[i] = c->roots[i + 1];
    c->root_count--;
    if (c->root_count == 0) { free(c->roots); c->roots = NULL; }
}

const char *config_command_for(const Config *c, const char *platform_id)
{
    if (!platform_id) return NULL;
    for (int i = 0; i < c->command_count; i++) {
        if (str_ieq(c->commands[i].platform_id, platform_id)) return c->commands[i].command;
    }
    return NULL;
}

void config_set_command(Config *c, const char *platform_id, const char *cmd)
{
    if (!platform_id) return;
    for (int i = 0; i < c->command_count; i++) {
        if (str_ieq(c->commands[i].platform_id, platform_id)) {
            free(c->commands[i].command);
            c->commands[i].command = xstrdup(cmd ? cmd : "");
            return;
        }
    }
    c->commands = xrealloc(c->commands, (size_t)(c->command_count + 1) * sizeof(PlatformCommand));
    c->commands[c->command_count].platform_id = xstrdup(platform_id);
    c->commands[c->command_count].command = xstrdup(cmd ? cmd : "");
    c->command_count++;
}

void config_remove_command(Config *c, const char *platform_id)
{
    if (!platform_id) return;
    for (int i = 0; i < c->command_count; i++) {
        if (!str_ieq(c->commands[i].platform_id, platform_id)) continue;
        free(c->commands[i].platform_id);
        free(c->commands[i].command);
        for (int j = i; j < c->command_count - 1; j++)
            c->commands[j] = c->commands[j + 1];
        c->command_count--;
        if (c->command_count == 0) {
            free(c->commands);
            c->commands = NULL;
        }
        return;
    }
}

static void add_default_roots(Config *c)
{
    const char *home = getenv("HOME");
    if (!home) return;
    const char *cands[] = {
        "ROMs", "Roms", "roms",
        "Emulation/roms", "Emulation/ROMs",
        "Downloads/ROMs", "Downloads/roms",
        "Games/ROMs",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        char *p = path_join(home, cands[i]);
        if (dir_exists(p)) config_add_root(c, p);
        free(p);
    }
}

void config_load(Config *c)
{
    char path[4096];
    config_file_path(path, sizeof(path));
    char *data = read_file(path, NULL);
    if (!data) {
        add_default_roots(c);
        config_save(c);
        return;
    }

    free_commands(c);
    for (int i = 0; i < c->root_count; i++) free(c->roots[i]);
    free(c->roots);
    c->roots = NULL;
    c->root_count = 0;

    bool auto_seen = false;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || *line == '#' || *line == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);
        if (strcmp(key, "root") == 0) config_add_root(c, val);
        else if (strcmp(key, "last_platform") == 0) { free(c->last_platform); c->last_platform = xstrdup(val); }
        else if (strcmp(key, "last_game") == 0) { free(c->last_game); c->last_game = xstrdup(val); }
        else if (strcmp(key, "swap_ab") == 0) c->swap_ab = atoi(val) != 0;
        else if (strcmp(key, "swap_xy") == 0) c->swap_xy = atoi(val) != 0;
        else if (strcmp(key, "swap_ps") == 0) c->swap_ps = atoi(val) != 0;
        else if (strcmp(key, "custom_mapping") == 0) c->custom_mapping = atoi(val) != 0;
        else if (strcmp(key, "map_confirm") == 0) c->map_confirm = atoi(val);
        else if (strcmp(key, "map_cancel") == 0) c->map_cancel = atoi(val);
        else if (strcmp(key, "map_menu1") == 0) c->map_menu1 = atoi(val);
        else if (strcmp(key, "map_menu2") == 0) c->map_menu2 = atoi(val);
        else if (strcmp(key, "language") == 0) c->language = atoi(val);
        else if (strcmp(key, "theme") == 0) c->theme = atoi(val);
        else if (strcmp(key, "theme_auto") == 0) c->theme_auto = atoi(val) != 0;
        else if (strcmp(key, "auto_dark_min") == 0) c->auto_dark_min = atoi(val);
        else if (strcmp(key, "auto_light_min") == 0) c->auto_light_min = atoi(val);
        else if (strcmp(key, "sound") == 0) c->sound = atoi(val) != 0;
        else if (strcmp(key, "volume") == 0) c->volume = atoi(val);
        else if (strcmp(key, "ui_scale") == 0) c->ui_scale = atoi(val);
        else if (strcmp(key, "ui_scale_auto") == 0) {
            c->ui_scale_auto = atoi(val) != 0;
            auto_seen = true;
        }
        else if (strcmp(key, "refresh_hz") == 0) {
            c->refresh_hz = atoi(val);
            if (c->refresh_hz != 60 && c->refresh_hz != 120)
                c->refresh_hz = 0;
        }
        else if (strcmp(key, "render_scale") == 0) {
            c->render_scale = atoi(val);
            if (c->render_scale < 50 || c->render_scale > 100)
                c->render_scale = 100;
        }
        else if (strcmp(key, "renderer") == 0) {
            if (strcmp(val, "opengles2") == 0 || strcmp(val, "gles") == 0 ||
                strcmp(val, "opengles") == 0)
                snprintf(c->renderer, sizeof(c->renderer), "opengles2");
            else if (strcmp(val, "vulkan") == 0)
                snprintf(c->renderer, sizeof(c->renderer), "vulkan");
            else if (strcmp(val, "software") == 0)
                snprintf(c->renderer, sizeof(c->renderer), "software");
            else
                snprintf(c->renderer, sizeof(c->renderer), "opengl");
        }
        else if (strcmp(key, "reduce_motion") == 0) c->reduce_motion = atoi(val) != 0;
        else if (strcmp(key, "grid_columns") == 0) c->grid_columns = atoi(val);
        else if (strcmp(key, "scan_on_start") == 0) c->scan_on_start = atoi(val) != 0;
        else if (strcmp(key, "show_cover_panel") == 0) c->show_cover_panel = atoi(val) != 0;
        else if (strcmp(key, "low_power") == 0) c->low_power = atoi(val) != 0;
        else if (strcmp(key, "app_mode") == 0) c->app_mode = atoi(val);
        else if (strcmp(key, "max_paused_apps") == 0) {
            c->max_paused_apps = atoi(val);
            if (c->max_paused_apps < 1) c->max_paused_apps = 1;
            if (c->max_paused_apps > 6) c->max_paused_apps = 6;
        }
        else if (strncmp(key, "command.", 8) == 0) {
            char *cmd = unescape_field(val);
            config_set_command(c, key + 8, cmd);
            free(cmd);
        }
    }
    free(data);
    if (c->ui_scale < 100) c->ui_scale = 100;
    if (c->ui_scale > 300) c->ui_scale = 300;
    /* Older configs only have ui_scale.  A value of 100 was the old default,
     * so treat it as "not customised" and enable auto scaling. */
    if (!auto_seen) c->ui_scale_auto = (c->ui_scale == 100);
    if (c->root_count == 0) add_default_roots(c);
}

void config_save(const Config *c)
{
    char path[4096];
    config_file_path(path, sizeof(path));
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    buf[0] = '\0';
#define APPEND(...) do { \
        int need = snprintf(NULL, 0, __VA_ARGS__); \
        while (len + (size_t)need + 1 > cap) { cap *= 2; buf = xrealloc(buf, cap); } \
        len += (size_t)snprintf(buf + len, cap - len, __VA_ARGS__); \
    } while (0)
    APPEND("# Taiji desktop settings\n");
    APPEND("last_platform=%s\n", c->last_platform ? c->last_platform : "all");
    APPEND("last_game=%s\n", c->last_game ? c->last_game : "");
    APPEND("swap_ab=%d\n", c->swap_ab ? 1 : 0);
    APPEND("swap_xy=%d\n", c->swap_xy ? 1 : 0);
    APPEND("swap_ps=%d\n", c->swap_ps ? 1 : 0);
    APPEND("custom_mapping=%d\n", c->custom_mapping ? 1 : 0);
    APPEND("map_confirm=%d\n", c->map_confirm);
    APPEND("map_cancel=%d\n", c->map_cancel);
    APPEND("map_menu1=%d\n", c->map_menu1);
    APPEND("map_menu2=%d\n", c->map_menu2);
    APPEND("language=%d\n", c->language);
    APPEND("theme=%d\n", c->theme);
    APPEND("theme_auto=%d\n", c->theme_auto ? 1 : 0);
    APPEND("auto_dark_min=%d\n", c->auto_dark_min);
    APPEND("auto_light_min=%d\n", c->auto_light_min);
    APPEND("sound=%d\n", c->sound ? 1 : 0);
    APPEND("volume=%d\n", c->volume);
    APPEND("ui_scale=%d\n", c->ui_scale);
    APPEND("ui_scale_auto=%d\n", c->ui_scale_auto ? 1 : 0);
    APPEND("refresh_hz=%d\n", c->refresh_hz);
    APPEND("render_scale=%d\n", c->render_scale);
    APPEND("renderer=%s\n", c->renderer[0] ? c->renderer : "opengles2");
    APPEND("reduce_motion=%d\n", c->reduce_motion ? 1 : 0);
    APPEND("grid_columns=%d\n", c->grid_columns);
    APPEND("scan_on_start=%d\n", c->scan_on_start ? 1 : 0);
    APPEND("show_cover_panel=%d\n", c->show_cover_panel ? 1 : 0);
    APPEND("low_power=%d\n", c->low_power ? 1 : 0);
    APPEND("app_mode=%d\n", c->app_mode);
    APPEND("max_paused_apps=%d\n", c->max_paused_apps);
    APPEND("store_pkg_type=%d\n", c->store_pkg_type);
    APPEND("store_pacman_source=%d\n", c->store_pacman_source);
    APPEND("store_flatpak_source=%d\n", c->store_flatpak_source);
    for (int i = 0; i < c->root_count; i++) APPEND("root=%s\n", c->roots[i]);
    for (int i = 0; i < c->command_count; i++) {
        char *esc = escape_field(c->commands[i].command);
        APPEND("command.%s=%s\n", c->commands[i].platform_id, esc);
        free(esc);
    }
#undef APPEND
    write_file_atomic(path, buf, len);
    free(buf);
}
