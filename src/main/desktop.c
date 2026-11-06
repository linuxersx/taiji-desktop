// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "desktop.h"
#include "i18n.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char name[160];
    char name_zh[160];
    char exec[1024];
    char icon[1024];
    char categories[512];
    char try_exec[512];
    char type[32];
    bool no_display;
    bool hidden;
    bool terminal;
    bool terminal_set;
} DesktopEntry;

static bool has_suffix_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    return str_ieq(s + n - m, suffix);
}

static void unescape_value(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 's': *w++ = ' '; break;
                case 'n': *w++ = '\n'; break;
                case 't': *w++ = '\t'; break;
                case 'r': *w++ = '\r'; break;
                case '\\': *w++ = '\\'; break;
                default: *w++ = *p; break;
            }
        } else {
            *w++ = *p;
        }
    }
    *w = '\0';
}

static void strip_exec_codes(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_size; i++) {
        if (in[i] == '%' && in[i + 1]) {
            char c = in[i + 1];
            i++;
            if (c == '%') out[j++] = '%';
            /* %f %F %u %U %d %D %n %N %i %c %k %v %m: no file argument in a
             * launcher menu; strip the field code. */
            continue;
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

static bool command_in_path(const char *cmd)
{
    if (!cmd || !*cmd) return false;
    if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/local/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = path_join(dir, cmd);
        bool ok = access(candidate, X_OK) == 0;
        free(candidate);
        if (ok) {
            free(copy);
            return true;
        }
    }
    free(copy);
    return false;
}

static bool icon_is_image(const char *path)
{
    return has_suffix_ci(path, ".png") || has_suffix_ci(path, ".jpg") ||
           has_suffix_ci(path, ".jpeg") || has_suffix_ci(path, ".bmp");
}

static bool find_icon_named(const char *name, char *out, size_t out_size)
{
    if (!name || !*name) return false;
    if (name[0] == '/') {
        if (file_exists(name) && icon_is_image(name)) {
            snprintf(out, out_size, "%s", name);
            return true;
        }
        return false;
    }

    const char *home = getenv("HOME");
    char *user_icons = home ? path_join(home, ".local/share/icons") : NULL;
    const char *roots[] = {
        user_icons, "/usr/local/share/icons", "/usr/share/icons",
        "/var/lib/flatpak/exports/share/icons",
        home ? "/home" : NULL
    };
    const char *themes[] = {"hicolor", "Adwaita", "Yaru", "breeze", "Papirus", "elementary", "gnome"};
    const char *sizes[] = {"256x256", "128x128", "64x64", "48x48", "32x32"};
    char base[256];
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && (str_ieq(dot, ".png") || str_ieq(dot, ".svg") || str_ieq(dot, ".xpm"))) *dot = '\0';

    char candidate[PATH_MAX];
    /* Prefer bitmap icons, largest first. */
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        if (!roots[r] || strncmp(roots[r], "/home", 5) == 0) continue;
        for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                snprintf(candidate, sizeof(candidate), "%s/%s/%s/apps/%s.png",
                         roots[r], themes[t], sizes[s], base);
                if (access(candidate, R_OK) == 0) {
                    snprintf(out, out_size, "%s", candidate);
                    free(user_icons);
                    return true;
                }
            }
        }
    }

    /* Pixmaps and theme legacy locations. */
    const char *pixmaps[] = {"/usr/share/pixmaps", "/usr/local/share/pixmaps", NULL};
    for (int i = 0; pixmaps[i]; i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s.png", pixmaps[i], base);
        if (access(candidate, R_OK) == 0) {
            snprintf(out, out_size, "%s", candidate);
            free(user_icons);
            return true;
        }
    }

    /* SVG fallback. The caller may rasterize it into the icon cache. */
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        if (!roots[r] || strncmp(roots[r], "/home", 5) == 0) continue;
        for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
            snprintf(candidate, sizeof(candidate), "%s/%s/scalable/apps/%s.svg",
                     roots[r], themes[t], base);
            if (access(candidate, R_OK) == 0) {
                snprintf(out, out_size, "%s", candidate);
                free(user_icons);
                return true;
            }
            snprintf(candidate, sizeof(candidate), "%s/%s/48x48/apps/%s.svg",
                     roots[r], themes[t], base);
            if (access(candidate, R_OK) == 0) {
                snprintf(out, out_size, "%s", candidate);
                free(user_icons);
                return true;
            }
        }
    }
    snprintf(candidate, sizeof(candidate), "/usr/share/pixmaps/%s.svg", base);
    if (access(candidate, R_OK) == 0) {
        snprintf(out, out_size, "%s", candidate);
        free(user_icons);
        return true;
    }

    /* KDE / breeze layout: <theme>/apps/<size>/<name>.(png|svg).
     * Appended after the existing hicolor/pixmaps/scalable lookups so their
     * priority is unchanged. All new bitmap candidates are checked before
     * the new SVG candidates. */
    static const char *kde_themes[] = {"breeze", "breeze-dark", "Adwaita"};
    static const char *kde_sizes[] = {"256", "128", "64", "48", "32", "24", "22", "16"};
    static const char *legacy_themes[] = {"AdwaitaLegacy", "Adwaita"};
    static const char *legacy_sizes[] = {"256x256", "128x128", "64x64", "48x48",
                                         "32x32", "24x24", "22x22", "16x16"};
    for (int pass = 0; pass < 2; pass++) {
        const char *ext = pass == 0 ? "png" : "svg";
        for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
            if (!roots[r] || strncmp(roots[r], "/home", 5) == 0) continue;
            for (size_t t = 0; t < sizeof(kde_themes) / sizeof(kde_themes[0]); t++) {
                for (size_t z = 0; z < sizeof(kde_sizes) / sizeof(kde_sizes[0]); z++) {
                    snprintf(candidate, sizeof(candidate), "%s/%s/apps/%s/%s.%s",
                             roots[r], kde_themes[t], kde_sizes[z], base, ext);
                    if (access(candidate, R_OK) == 0) {
                        snprintf(out, out_size, "%s", candidate);
                        free(user_icons);
                        return true;
                    }
                }
            }
            for (size_t t = 0; t < sizeof(legacy_themes) / sizeof(legacy_themes[0]); t++) {
                for (size_t z = 0; z < sizeof(legacy_sizes) / sizeof(legacy_sizes[0]); z++) {
                    snprintf(candidate, sizeof(candidate), "%s/%s/%s/legacy/%s.%s",
                             roots[r], legacy_themes[t], legacy_sizes[z], base, ext);
                    if (access(candidate, R_OK) == 0) {
                        snprintf(out, out_size, "%s", candidate);
                        free(user_icons);
                        return true;
                    }
                }
            }
        }
    }

    free(user_icons);
    return false;
}

static bool run_converter(const char *prog, const char *svg, const char *png)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (strcmp(prog, "rsvg-convert") == 0)
            execlp(prog, prog, "-w", "128", "-h", "128", "-o", png, svg, (char *)NULL);
        else if (strcmp(prog, "magick") == 0)
            execlp(prog, prog, "convert", "-background", "none", "-resize", "128x128", svg, png, (char *)NULL);
        else
            execlp(prog, prog, "-background", "none", "-resize", "128x128", svg, png, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && file_exists(png);
}

static bool cache_icon_path(const char *src, const char *prefix,
                            char *out, size_t out_size, bool create_dir)
{
    char *cache_dir = path_join(taiji_config_dir(), "icons");
    if (create_dir && !mkdir_p(cache_dir)) {
        free(cache_dir);
        return false;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s-%08x.png", prefix, fnv1a(src));
    char *png = path_join(cache_dir, name);
    free(cache_dir);
    if (file_exists(png)) {
        snprintf(out, out_size, "%s", png);
        free(png);
        return true;
    }
    if (out_size) out[0] = '\0';
    if (create_dir) snprintf(out, out_size, "%s", png);
    free(png);
    return false;
}

bool desktop_icon_cached_png(const char *icon, char *out, size_t out_size)
{
    if (!icon || !*icon || !out || out_size == 0) return false;
    return cache_icon_path(icon, "icon", out, out_size, false);
}

static bool cache_raster_icon(const char *src, const char *prefix,
                              bool allow_rsvg, char *out, size_t out_size)
{
    char *cache_dir = path_join(taiji_config_dir(), "icons");
    if (!mkdir_p(cache_dir)) {
        free(cache_dir);
        return false;
    }
    char name[64];
    snprintf(name, sizeof(name), "%s-%08x.png", prefix, fnv1a(src));
    char *png = path_join(cache_dir, name);
    free(cache_dir);
    if (file_exists(png)) {
        snprintf(out, out_size, "%s", png);
        free(png);
        return true;
    }

    const char *converters[] = {"rsvg-convert", "magick", "convert"};
    for (size_t i = 0; i < sizeof(converters) / sizeof(converters[0]); i++) {
        if (!allow_rsvg && strcmp(converters[i], "rsvg-convert") == 0) continue;
        if (command_in_path(converters[i]) && run_converter(converters[i], src, png)) {
            snprintf(out, out_size, "%s", png);
            free(png);
            return true;
        }
    }
    free(png);
    return false;
}

static bool cache_svg_icon(const char *svg, char *out, size_t out_size)
{
    if (!svg || !has_suffix_ci(svg, ".svg")) return false;
    return cache_raster_icon(svg, "desktop", true, out, out_size);
}

bool desktop_icon_to_png(const char *icon, char *out, size_t out_size)
{
    if (!icon || !*icon || !out || out_size == 0) return false;
    out[0] = '\0';
    if (!file_exists(icon)) return false;
    if (has_suffix_ci(icon, ".svg"))
        return cache_raster_icon(icon, "icon", true, out, out_size);
    if (has_suffix_ci(icon, ".xpm"))
        return cache_raster_icon(icon, "icon", false, out, out_size);
    snprintf(out, out_size, "%s", icon);
    return true;
}

static void resolve_icon(const char *icon, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!icon || !*icon) return;
    if (icon[0] == '/' && file_exists(icon)) {
        if (icon_is_image(icon)) {
            snprintf(out, out_size, "%s", icon);
        } else if (has_suffix_ci(icon, ".svg")) {
            cache_svg_icon(icon, out, out_size);
        }
        return;
    }
    char found[PATH_MAX];
    if (find_icon_named(icon, found, sizeof(found))) {
        if (has_suffix_ci(found, ".svg")) cache_svg_icon(found, out, out_size);
        else snprintf(out, out_size, "%s", found);
    }
}

static const char *map_categories(const char *categories)
{
    if (!categories) return "other";
    if (strstr(categories, "Game")) return "game";
    if (strstr(categories, "AudioVideo") || strstr(categories, "Audio") ||
        strstr(categories, "Video") || strstr(categories, "Player")) return "media";
    if (strstr(categories, "Graphics")) return "graphics";
    if (strstr(categories, "Office")) return "office";
    if (strstr(categories, "Development")) return "development";
    if (strstr(categories, "Network")) return "network";
    if (strstr(categories, "System") || strstr(categories, "Settings") ||
        strstr(categories, "Administration")) return "system";
    if (strstr(categories, "Utility")) return "utility";
    return "other";
}

static bool parse_desktop(const char *path, DesktopEntry *e)
{
    memset(e, 0, sizeof(*e));
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return false;
    bool in_main = false;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || *line == '#') continue;
        if (*line == '[') {
            in_main = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_main) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);
        unescape_value(val);
        if (strcmp(key, "Type") == 0) snprintf(e->type, sizeof(e->type), "%s", val);
        else if (strcmp(key, "Name") == 0) snprintf(e->name, sizeof(e->name), "%s", val);
        else if (strcmp(key, "Name[zh_CN]") == 0) snprintf(e->name_zh, sizeof(e->name_zh), "%s", val);
        else if (strcmp(key, "Name[zh]") == 0 && !e->name_zh[0]) snprintf(e->name_zh, sizeof(e->name_zh), "%s", val);
        else if (strcmp(key, "Exec") == 0) snprintf(e->exec, sizeof(e->exec), "%s", val);
        else if (strcmp(key, "Icon") == 0) snprintf(e->icon, sizeof(e->icon), "%s", val);
        else if (strcmp(key, "Categories") == 0) snprintf(e->categories, sizeof(e->categories), "%s", val);
        else if (strcmp(key, "TryExec") == 0) snprintf(e->try_exec, sizeof(e->try_exec), "%s", val);
        else if (strcmp(key, "NoDisplay") == 0) e->no_display = str_ieq(val, "true");
        else if (strcmp(key, "Hidden") == 0) e->hidden = str_ieq(val, "true");
        else if (strcmp(key, "Terminal") == 0) {
            e->terminal = str_ieq(val, "true");
            e->terminal_set = true;
        }
    }
    free(data);
    return e->type[0] && str_ieq(e->type, "Application") && e->exec[0] &&
           !e->no_display && !e->hidden;
}

static AppEntry *find_by_desktop_file(AppList *list, const char *path)
{
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->apps[i].desktop_file, path) == 0) return &list->apps[i];
    }
    return NULL;
}

static bool import_file(AppList *list, const char *path, DesktopImportStats *stats)
{
    DesktopEntry e;
    if (!parse_desktop(path, &e)) {
        stats->skipped++;
        return false;
    }
    if (e.try_exec[0] && !command_in_path(e.try_exec)) {
        stats->skipped++;
        return false;
    }
    if (has_suffix_ci(path, "/taiji.desktop")) {
        stats->skipped++;
        return false;
    }

    char command[2048];
    strip_exec_codes(e.exec, command, sizeof(command));

    char icon_path[1024] = {0};
    resolve_icon(e.icon, icon_path, sizeof(icon_path));

    const char *name = (i18n_language() != 1 && e.name_zh[0]) ? e.name_zh : e.name;
    if (!name[0]) name = path_basename(path);

    AppEntry *a = find_by_desktop_file(list, path);
    bool added = false;
    if (!a) {
        if (list->count >= (int)(sizeof(list->apps) / sizeof(list->apps[0]))) return false;
        unsigned h = fnv1a(path);
        char id[64];
        snprintf(id, sizeof(id), "desktop-%08x", h);
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->apps[i].id, id) == 0) {
                snprintf(id, sizeof(id), "desktop-%08x-%d", h, i);
                break;
            }
        }
        a = &list->apps[list->count];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", id);
        a->mode = APP_MODE_AUTO;
        a->on_return = APP_ON_RETURN_PAUSE;
        a->backend = APP_BACKEND_AUTO;
        snprintf(a->desktop_file, sizeof(a->desktop_file), "%s", path);
        list->count++;
        added = true;
    }

    char new_name[sizeof(a->name)];
    char new_command[sizeof(a->command)];
    char new_icon[sizeof(a->icon)];
    char new_category[sizeof(a->category)];
    snprintf(new_name, sizeof(new_name), "%.*s", (int)sizeof(new_name) - 1, name);
    snprintf(new_command, sizeof(new_command), "%.*s", (int)sizeof(new_command) - 1, command);
    snprintf(new_icon, sizeof(new_icon), "%.*s", (int)sizeof(new_icon) - 1, icon_path);
    snprintf(new_category, sizeof(new_category), "%s", map_categories(e.categories));

    /* A periodic rescan must not report a change when nothing changed, or the
     * app badge lights up and apps.json gets rewritten every few seconds. */
    int new_terminal = e.terminal_set
                           ? (e.terminal ? APP_TERMINAL_YES : APP_TERMINAL_NO)
                           : APP_TERMINAL_AUTO;
    int new_return = a->on_return_custom ? a->on_return :
                     (strcmp(new_category, "media") == 0 ?
                      APP_ON_RETURN_RUN : APP_ON_RETURN_PAUSE);
    bool changed = added;
    bool terminal_changed = !a->terminal_custom && a->terminal != new_terminal;
    if (!added &&
        (strcmp(a->name, new_name) != 0 ||
         strcmp(a->command, new_command) != 0 ||
         strcmp(a->icon, new_icon) != 0 ||
         strcmp(a->category, new_category) != 0 ||
         terminal_changed ||
         a->on_return != new_return)) {
        changed = true;
    }
    if (changed) {
        snprintf(a->name, sizeof(a->name), "%s", new_name);
        snprintf(a->command, sizeof(a->command), "%s", new_command);
        snprintf(a->icon, sizeof(a->icon), "%s", new_icon);
        snprintf(a->category, sizeof(a->category), "%s", new_category);
        if (!a->terminal_custom) a->terminal = new_terminal;
        a->on_return = new_return;
    }
    if (added) stats->added++;
    else if (changed) stats->updated++;
    return true;
}

static void scan_dir(AppList *list, const char *dir, DesktopImportStats *stats)
{
    int count = 0;
    char **items = list_dir(dir, &count);
    if (!items) return;
    for (int i = 0; i < count; i++) {
        if (has_suffix_ci(items[i], ".desktop")) {
            char *path = path_join(dir, items[i]);
            import_file(list, path, stats);
            free(path);
        }
    }
    free_strv(items, count);
}

#define PENDING_GONE_MAX 64
typedef struct {
    char path[PATH_MAX];
    int64_t first_ms;
} PendingGone;

static PendingGone g_pending_gone[PENDING_GONE_MAX];
static int g_pending_gone_count;

static int64_t desktop_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int pending_gone_find(const char *path)
{
    for (int i = 0; i < g_pending_gone_count; i++)
        if (strcmp(g_pending_gone[i].path, path) == 0) return i;
    return -1;
}

static void pending_gone_clear(const char *path)
{
    int i = pending_gone_find(path);
    if (i < 0) return;
    for (int j = i; j < g_pending_gone_count - 1; j++)
        g_pending_gone[j] = g_pending_gone[j + 1];
    g_pending_gone_count--;
}

static void pending_gone_touch(const char *path)
{
    if (pending_gone_find(path) >= 0) return;
    if (g_pending_gone_count >= PENDING_GONE_MAX) return;
    PendingGone *p = &g_pending_gone[g_pending_gone_count++];
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->first_ms = desktop_now_ms();
}

DesktopImportStats desktop_import_apps(AppList *list)
{
    DesktopImportStats stats = {0};
    const char *home = getenv("HOME");
    scan_dir(list, "/usr/share/applications", &stats);
    scan_dir(list, "/usr/local/share/applications", &stats);
    if (home) {
        char *user = path_join(home, ".local/share/applications");
        scan_dir(list, user, &stats);
        free(user);
        user = path_join(home, ".local/share/flatpak/exports/share/applications");
        scan_dir(list, user, &stats);
        free(user);
    }
    scan_dir(list, "/var/lib/flatpak/exports/share/applications", &stats);
    scan_dir(list, "/var/lib/snapd/desktop/applications", &stats);

    /* Remove imported entries whose .desktop file is gone.  Only a definite
     * ENOENT/ENOTDIR counts; permission errors keep the entry. */
    int64_t now = desktop_now_ms();
    for (int i = 0; i < list->count;) {
        AppEntry *a = &list->apps[i];
        bool gone = false;
        if (a->desktop_file[0]) {
            struct stat st;
            if (stat(a->desktop_file, &st) == 0) {
                pending_gone_clear(a->desktop_file);
            } else if (errno == ENOENT || errno == ENOTDIR) {
                int p = pending_gone_find(a->desktop_file);
                if (p >= 0 && now - g_pending_gone[p].first_ms >= 2500) {
                    gone = true;
                } else if (p < 0) {
                    pending_gone_touch(a->desktop_file);
                }
            }
        }
        if (gone) {
            pending_gone_clear(a->desktop_file);
            for (int j = i; j < list->count - 1; j++)
                list->apps[j] = list->apps[j + 1];
            list->count--;
            memset(&list->apps[list->count], 0, sizeof(list->apps[0]));
            stats.removed++;
            continue;
        }
        i++;
    }
    if (stats.added || stats.updated || stats.removed) apps_save(list);
    return stats;
}

static char *import_marker(void)
{
    return path_join(taiji_config_dir(), ".desktop-imported");
}

bool desktop_import_done(void)
{
    char *path = import_marker();
    bool done = file_exists(path);
    free(path);
    return done;
}

void desktop_mark_imported(void)
{
    char *path = import_marker();
    write_file_atomic(path, "1\n", 2);
    free(path);
}

DesktopImportStats desktop_import_auto(AppList *list)
{
    DesktopImportStats empty = {0};
    if (desktop_import_done()) return empty;
    DesktopImportStats stats = desktop_import_apps(list);
    desktop_mark_imported();
    return stats;
}
