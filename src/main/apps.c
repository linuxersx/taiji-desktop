// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "apps.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void apps_init(AppList *list)
{
    memset(list, 0, sizeof(*list));
}

void apps_free(AppList *list)
{
    memset(list, 0, sizeof(*list));
}

static char *apps_path(void)
{
    return path_join(taiji_config_dir(), "apps.tsv");
}

static int mode_from_name(const char *s)
{
    if (!s) return APP_MODE_AUTO;
    if (str_ieq(s, "resident")) return APP_MODE_RESIDENT;
    if (str_ieq(s, "handoff")) return APP_MODE_HANDOFF;
    return APP_MODE_AUTO;
}

static int return_from_name(const char *s)
{
    if (!s) return APP_ON_RETURN_PAUSE;
    if (str_ieq(s, "run")) return APP_ON_RETURN_RUN;
    if (str_ieq(s, "close")) return APP_ON_RETURN_CLOSE;
    return APP_ON_RETURN_PAUSE;
}

static int backend_from_name(const char *s)
{
    if (!s) return APP_BACKEND_AUTO;
    if (str_ieq(s, "wayland")) return APP_BACKEND_WAYLAND;
    if (str_ieq(s, "x11")) return APP_BACKEND_X11;
    return APP_BACKEND_AUTO;
}

void apps_load(AppList *list)
{
    apps_init(list);
    char *path = apps_path();
    size_t len = 0;
    char *data = read_file(path, &len);
    free(path);
    if (!data) return;

    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (!*line || *line == '#') continue;
        if (list->count >= (int)(sizeof(list->apps) / sizeof(list->apps[0]))) break;

        char *fields[16] = {0};
        int n = 0;
        char *p = line;
        while (n < 16) {
            fields[n++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n < 2) continue;

        AppEntry *a = &list->apps[list->count];
        memset(a, 0, sizeof(*a));
        char *v = unescape_field(fields[0]);
        snprintf(a->id, sizeof(a->id), "%s", v ? v : "");
        free(v);
        v = unescape_field(n > 1 && fields[1] ? fields[1] : "");
        snprintf(a->name, sizeof(a->name), "%s", v ? v : "");
        free(v);
        v = unescape_field(n > 2 && fields[2] ? fields[2] : "");
        snprintf(a->command, sizeof(a->command), "%s", v ? v : "");
        free(v);
        v = unescape_field(n > 3 && fields[3] ? fields[3] : "");
        snprintf(a->icon, sizeof(a->icon), "%s", v ? v : "");
        free(v);
        a->mode = mode_from_name(n > 4 ? fields[4] : NULL);
        a->on_return = return_from_name(n > 5 ? fields[5] : NULL);
        a->backend = backend_from_name(n > 6 ? fields[6] : NULL);
        a->heavy = n > 7 ? atoi(fields[7]) != 0 : false;
        a->background = n > 8 ? atoi(fields[8]) != 0 : false;
        a->startup = n > 9 ? atoi(fields[9]) != 0 : false;
        v = unescape_field(n > 10 && fields[10] ? fields[10] : "other");
        snprintf(a->category, sizeof(a->category), "%s", v && *v ? v : "other");
        free(v);
        v = unescape_field(n > 11 && fields[11] ? fields[11] : "");
        snprintf(a->desktop_file, sizeof(a->desktop_file), "%s", v ? v : "");
        free(v);
        a->terminal = n > 12 ? atoi(fields[12]) : APP_TERMINAL_AUTO;
        if (a->terminal < APP_TERMINAL_AUTO || a->terminal > APP_TERMINAL_NO)
            a->terminal = APP_TERMINAL_AUTO;
        a->on_return_custom = n > 13 ? atoi(fields[13]) != 0 : false;
        a->terminal_custom = n > 14 ? atoi(fields[14]) != 0 : false;
        if (!a->id[0]) {
            snprintf(a->id, sizeof(a->id), "app-%ld-%d", (long)time(NULL), list->count);
        }
        if (!a->name[0]) {
            char idbuf[64];
            snprintf(idbuf, sizeof(idbuf), "%s", a->id);
            snprintf(a->name, sizeof(a->name), "%s", idbuf);
        }
        if (!a->command[0]) snprintf(a->command, sizeof(a->command), "true");
        list->count++;
    }
    free(data);
}

bool apps_save(const AppList *list)
{
    size_t cap = 4096;
    size_t used = 0;
    char *buf = xmalloc(cap);
    buf[0] = '\0';

    for (int i = 0; i < list->count; i++) {
        const AppEntry *a = &list->apps[i];
        char *id = escape_field(a->id);
        char *name = escape_field(a->name);
        char *cmd = escape_field(a->command);
        char *icon = escape_field(a->icon);
        char *cat = escape_field(a->category[0] ? a->category : "other");
        char *dfile = escape_field(a->desktop_file);
        char *line = xasprintf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\n",
                               id, name, cmd, icon,
                               apps_mode_name(a->mode), apps_return_name(a->on_return),
                               apps_backend_name(a->backend), a->heavy ? 1 : 0,
                               a->background ? 1 : 0, a->startup ? 1 : 0, cat, dfile,
                               a->terminal, a->on_return_custom ? 1 : 0,
                               a->terminal_custom ? 1 : 0);
        size_t need = strlen(line);
        if (used + need + 1 > cap) {
            while (used + need + 1 > cap) cap *= 2;
            buf = xrealloc(buf, cap);
        }
        memcpy(buf + used, line, need);
        used += need;
        buf[used] = '\0';
        free(line);
        free(id); free(name); free(cmd); free(icon); free(cat); free(dfile);
    }

    char *path = apps_path();
    bool ok = write_file_atomic(path, buf, used);
    free(path);
    free(buf);
    return ok;
}

int apps_add(AppList *list, const char *name, const char *command)
{
    if (list->count >= (int)(sizeof(list->apps) / sizeof(list->apps[0]))) return -1;
    AppEntry *a = &list->apps[list->count];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "app-%ld-%d", (long)time(NULL), list->count);
    snprintf(a->name, sizeof(a->name), "%s", name && *name ? name : "新应用");
    snprintf(a->command, sizeof(a->command), "%s", command && *command ? command : "true");
    a->mode = APP_MODE_AUTO;
    a->on_return = APP_ON_RETURN_PAUSE;
    a->backend = APP_BACKEND_AUTO;
    snprintf(a->category, sizeof(a->category), "other");
    list->count++;
    apps_save(list);
    return list->count - 1;
}

void apps_remove(AppList *list, int index)
{
    if (index < 0 || index >= list->count) return;
    for (int i = index; i + 1 < list->count; i++) list->apps[i] = list->apps[i + 1];
    list->count--;
    memset(&list->apps[list->count], 0, sizeof(list->apps[0]));
    apps_save(list);
}

AppEntry *apps_find(AppList *list, const char *id)
{
    if (!list || !id) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->apps[i].id, id) == 0) return &list->apps[i];
    }
    return NULL;
}

const AppEntry *apps_find_const(const AppList *list, const char *id)
{
    return apps_find((AppList *)list, id);
}

const char *apps_mode_name(int mode)
{
    if (mode == APP_MODE_RESIDENT) return "resident";
    if (mode == APP_MODE_HANDOFF) return "handoff";
    return "auto";
}

const char *apps_return_name(int on_return)
{
    if (on_return == APP_ON_RETURN_RUN) return "run";
    if (on_return == APP_ON_RETURN_CLOSE) return "close";
    return "pause";
}

const char *apps_backend_name(int backend)
{
    if (backend == APP_BACKEND_WAYLAND) return "wayland";
    if (backend == APP_BACKEND_X11) return "x11";
    return "auto";
}

int apps_cycle_mode(int mode, int dir)
{
    int v = (mode + dir) % 3;
    if (v < 0) v += 3;
    return v;
}

int apps_cycle_return(int on_return, int dir)
{
    int v = (on_return + dir) % 3;
    if (v < 0) v += 3;
    return v;
}

int apps_cycle_backend(int backend, int dir)
{
    int v = (backend + dir) % 3;
    if (v < 0) v += 3;
    return v;
}

static const char *CATEGORY_IDS[] = {
    "all", "game", "media", "graphics", "office", "development",
    "network", "utility", "system", "other"
};
static const char *CATEGORY_LABELS[] = {
    "全部", "游戏", "影音", "图形", "办公", "开发",
    "网络", "工具", "系统", "其他"
};

int apps_category_count(void)
{
    return (int)(sizeof(CATEGORY_IDS) / sizeof(CATEGORY_IDS[0]));
}

const char *apps_category_id(int index)
{
    if (index < 0 || index >= apps_category_count()) return "all";
    return CATEGORY_IDS[index];
}

const char *apps_category_label(int index)
{
    if (index < 0 || index >= apps_category_count()) return "全部";
    return CATEGORY_LABELS[index];
}

int apps_category_index(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < apps_category_count(); i++) {
        if (strcmp(CATEGORY_IDS[i], id) == 0) return i;
    }
    return (int)(sizeof(CATEGORY_IDS) / sizeof(CATEGORY_IDS[0])) - 1;
}

int apps_cycle_category(const char *id, int dir)
{
    int n = apps_category_count();
    int idx = apps_category_index(id);
    /* "all" is only a view, not an editable app category. */
    if (idx == 0) idx = 1;
    idx = 1 + ((idx - 1 + dir + (n - 1)) % (n - 1));
    return idx;
}

bool apps_category_matches(const AppEntry *entry, const char *category)
{
    if (!category || !*category || strcmp(category, "all") == 0) return true;
    return entry && strcmp(entry->category, category) == 0;
}

char *apps_runtime_path(void)
{
    const char *override = getenv("TAIJI_STATE_FILE");
    if (override && *override) return xstrdup(override);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) {
        char *dir = path_join(runtime, "taiji");
        char *path = path_join(dir, "state");
        free(dir);
        return path;
    }
    return path_join(taiji_config_dir(), "state");
}

char *apps_log_path(const char *id)
{
    char *dir = path_join(taiji_config_dir(), "logs");
    mkdir_p(dir);
    char *name = xasprintf("%s.log", id && *id ? id : "app");
    char *path = path_join(dir, name);
    free(name);
    free(dir);
    return path;
}

void apps_runtime_read(AppRuntime *rt)
{
    memset(rt, 0, sizeof(*rt));
    snprintf(rt->foreground, sizeof(rt->foreground), "taiji");
    char *path = apps_runtime_path();
    size_t len = 0;
    char *data = read_file(path, &len);
    free(path);
    if (!data) return;

    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);
        if (strcmp(key, "foreground") == 0) {
            snprintf(rt->foreground, sizeof(rt->foreground), "%s", val);
        } else if (strcmp(key, "app_id") == 0) {
            snprintf(rt->app_id, sizeof(rt->app_id), "%s", val);
        } else if (strcmp(key, "app_state") == 0) {
            snprintf(rt->app_state, sizeof(rt->app_state), "%s", val);
        } else if (strcmp(key, "on_return") == 0) {
            snprintf(rt->on_return, sizeof(rt->on_return), "%s", val);
        } else if (strcmp(key, "pid") == 0 || strcmp(key, "app_pid") == 0) {
            rt->pid = (pid_t)atol(val);
        } else if (strcmp(key, "pgid") == 0 || strcmp(key, "app_pgid") == 0) {
            rt->pgid = (pid_t)atol(val);
        }
    }
    free(data);
}

bool apps_runtime_write(const AppRuntime *rt)
{
    char *path = apps_runtime_path();
    char *dir = path_dirname_dup(path);
    if (dir && *dir) {
        mkdir_p(dir);
        chmod(dir, 0700);
    }
    free(dir);
    char *data = xasprintf(
        "foreground=%s\napp_id=%s\napp_state=%s\non_return=%s\npid=%ld\npgid=%ld\n",
        rt->foreground, rt->app_id, rt->app_state, rt->on_return,
        (long)rt->pid, (long)rt->pgid);
    bool ok = write_file_atomic(path, data, strlen(data));
    if (ok) chmod(path, 0600);
    free(data);
    free(path);
    return ok;
}

bool apps_runtime_alive(const AppRuntime *rt)
{
    if (!rt || rt->pid <= 0) return false;
    if (kill(rt->pid, 0) == 0) return true;
    return errno == EPERM;
}

const char *apps_runtime_state_label(const AppRuntime *rt, const AppEntry *entry)
{
    if (!entry) return "未运行";
    if (!rt || strcmp(rt->app_id, entry->id) != 0) return "未运行";
    if (strcmp(rt->foreground, "app") == 0) return "前台";
    if (strcmp(rt->app_state, "paused") == 0) return "暂停";
    if (strcmp(rt->app_state, "background") == 0) return "后台";
    if (strcmp(rt->app_state, "launching") == 0) return "启动";
    if (strcmp(rt->app_state, "failed") == 0) return "失败";
    return "空闲";
}
