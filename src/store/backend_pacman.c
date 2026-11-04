// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "store.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char name[128];
    char repo[64];
    char version[64];
    char description[STORE_TEXT_MAX];
    char license[64];
    char groups[256];
    long long size_kb;
} PkgInfo;

typedef struct {
    char **items;
    int count;
} NameList;

typedef struct {
    char name[128];
    char version[64];
} InstalledInfo;

static bool command_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    char *copy = xstrdup(path);
    char *save = NULL;
    bool found = false;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found;
         dir = strtok_r(NULL, ":", &save)) {
        char *p = path_join(dir, name);
        found = access(p, X_OK) == 0;
        free(p);
    }
    free(copy);
    return found;
}

static char *run_capture(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(f); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 4096) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(f); return NULL; }
            buf = nb;
        }
    }
    buf[len] = '\0';
    pclose(f);
    return buf;
}

static int cmp_info(const void *a, const void *b)
{
    return strcmp(((const PkgInfo *)a)->name, ((const PkgInfo *)b)->name);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool parse_size(const char *s, long long *out)
{
    double v = 0;
    char unit[16] = "";
    if (sscanf(s, "%lf %15s", &v, unit) < 1) return false;
    if (strncasecmp(unit, "GiB", 3) == 0) v *= 1024 * 1024;
    else if (strncasecmp(unit, "MiB", 3) == 0) v *= 1024;
    else if (strncasecmp(unit, "KiB", 3) == 0) { /* KiB */ }
    else if (strncasecmp(unit, "B", 1) == 0) v /= 1024;
    *out = (long long)v;
    return true;
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

static void parse_si(const char *text, PkgInfo **out, int *count)
{
    *out = NULL;
    *count = 0;
    int cap = 0;
    PkgInfo cur;
    memset(&cur, 0, sizeof(cur));
    bool have = false;
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (!line[0]) {
            if (have && cur.name[0]) {
                if (*count == cap) {
                    cap = cap ? cap * 2 : 128;
                    *out = realloc(*out, (size_t)cap * sizeof(PkgInfo));
                }
                (*out)[(*count)++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            have = false;
        } else {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *key = line;
                char *val = colon + 1;
                trim(key);
                trim(val);
                have = true;
                if (strcmp(key, "Name") == 0)
                    snprintf(cur.name, sizeof(cur.name), "%s", val);
                else if (strcmp(key, "Repository") == 0)
                    snprintf(cur.repo, sizeof(cur.repo), "%s", val);
                else if (strcmp(key, "Version") == 0)
                    snprintf(cur.version, sizeof(cur.version), "%s", val);
                else if (strcmp(key, "Description") == 0)
                    snprintf(cur.description, sizeof(cur.description), "%s", val);
                else if (strcmp(key, "Licenses") == 0)
                    snprintf(cur.license, sizeof(cur.license), "%s", val);
                else if (strcmp(key, "Groups") == 0)
                    snprintf(cur.groups, sizeof(cur.groups), "%s", val);
                else if (strcmp(key, "Installed Size") == 0)
                    parse_size(val, &cur.size_kb);
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (have && cur.name[0]) {
        if (*count == cap) {
            cap = cap ? cap + 1 : 1;
            *out = realloc(*out, (size_t)cap * sizeof(PkgInfo));
        }
        (*out)[(*count)++] = cur;
    }
    if (*out) qsort(*out, (size_t)*count, sizeof(PkgInfo), cmp_info);
}

static PkgInfo *find_info(PkgInfo *list, int count, const char *name)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(list[mid].name, name);
        if (cmp == 0) return &list[mid];
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static int cmp_installed(const void *a, const void *b)
{
    return strcmp(((const InstalledInfo *)a)->name,
                  ((const InstalledInfo *)b)->name);
}

static InstalledInfo *parse_installed(const char *text, int *count)
{
    *count = 0;
    if (!text) return NULL;
    int cap = 0;
    InstalledInfo *out = NULL;
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        char *sp = NULL;
        char *name = strtok_r(line, " \t", &sp);
        char *version = strtok_r(NULL, " \t", &sp);
        if (name && version) {
            if (*count == cap) {
                cap = cap ? cap * 2 : 128;
                out = realloc(out, (size_t)cap * sizeof(InstalledInfo));
            }
            snprintf(out[*count].name, sizeof(out[*count].name), "%s", name);
            snprintf(out[*count].version, sizeof(out[*count].version), "%s", version);
            (*count)++;
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (out) qsort(out, (size_t)*count, sizeof(InstalledInfo), cmp_installed);
    return out;
}

static const char *installed_version(InstalledInfo *list, int count, const char *name)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(list[mid].name, name);
        if (cmp == 0) return list[mid].version;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static NameList parse_names(const char *text, int field)
{
    NameList list = {0};
    if (!text) return list;
    int cap = 0;
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        char *save = NULL;
        char *tok = strtok_r(line, " \t", &save);
        for (int f = 0; tok && f < field; f++)
            tok = strtok_r(NULL, " \t", &save);
        if (tok && *tok) {
            if (list.count == cap) {
                cap = cap ? cap * 2 : 128;
                list.items = realloc(list.items, (size_t)cap * sizeof(char *));
            }
            list.items[list.count++] = xstrdup(tok);
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (list.items) qsort(list.items, (size_t)list.count, sizeof(char *), cmp_str);
    return list;
}

static bool name_in_list(NameList *list, const char *name)
{
    if (!list->items) return false;
    return bsearch(&name, list->items, (size_t)list->count, sizeof(char *), cmp_str) != NULL;
}

static void name_list_free(NameList *list)
{
    for (int i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static const char *category_for(const PkgInfo *info)
{
    const char *g = info->groups;
    if (strstr(g, "game")) return "games";
    if (strstr(g, "development") || strstr(g, "devel")) return "development";
    if (strstr(g, "multimedia") || strstr(g, "sound") || strstr(g, "video"))
        return "media";
    if (strstr(g, "network") || strstr(g, "internet")) return "network";
    if (strstr(g, "system") || strstr(g, "base")) return "system";
    if (strstr(g, "xorg") || strstr(g, "gnome") || strstr(g, "kde"))
        return "desktop";
    return "utility";
}

static void find_icon(const char *name, char *out, size_t n)
{
    out[0] = '\0';
    static const char *sizes[] = {"scalable", "256x256", "128x128", "64x64", "48x48"};
    static const char *exts[] = {".svg", ".png"};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]) && !out[0]; s++) {
        for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]) && !out[0]; e++) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "/usr/share/icons/hicolor/%s/apps/%s%s",
                     sizes[s], name, exts[e]);
            if (access(p, R_OK) == 0) snprintf(out, n, "%s", p);
        }
    }
    if (!out[0]) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "/usr/share/pixmaps/%s.png", name);
        if (access(p, R_OK) == 0) snprintf(out, n, "%s", p);
    }
}

static bool is_protected(const char *id)
{
    /* 固件绝对禁止；内核和启动相关包也默认保护。 */
    static const char *exact[] = {
        "linux", "linux-lts", "linux-zen", "linux-hardened",
        "linux-firmware", "linux-firmware-whence", "linux-api-headers",
        "alsa-firmware", "sof-firmware", "arm-trusted-firmware",
        "u-boot", "uboot", "grub", "systemd-boot", "edk2-ovmf"
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (strcmp(id, exact[i]) == 0) return true;
    static const char *prefix[] = {
        "linux", "firmware", "u-boot", "uboot", "lk-", "atf-", "boot", "grub"
    };
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (strncmp(id, prefix[i], strlen(prefix[i])) == 0) return true;
    if (strstr(id, "firmware")) return true;
    return false;
}

static bool pacman_probe(char *error, size_t error_size)
{
    if (!command_exists("pacman")) {
        if (error) snprintf(error, error_size, "未找到 pacman");
        return false;
    }
    if (access(store_helper_path(), X_OK) != 0 &&
        getenv("TAIJI_STORE_HELPER") == NULL) {
        if (error) snprintf(error, error_size, "未找到商店 helper");
        return false;
    }
    return true;
}

static bool pacman_refresh(StoreBackend *self, StorePackageList *out,
                           StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    if (progress && progress->progress)
        progress->progress(progress->ud, "正在读取仓库...", 10, NULL);
    char *sl = run_capture("LC_ALL=C pacman -Sl 2>/dev/null");
    if (!sl) {
        if (error) snprintf(error, error_size, "无法读取 pacman 仓库");
        return false;
    }
    if (progress && progress->progress)
        progress->progress(progress->ud, "正在读取包详情...", 35, NULL);
    char *si = run_capture("LC_ALL=C pacman -Si 2>/dev/null");
    char *q = run_capture("LC_ALL=C pacman -Q 2>/dev/null");
    char *qu = run_capture("LC_ALL=C pacman -Qu 2>/dev/null");
    if (!si) {
        free(sl); free(q); free(qu);
        if (error) snprintf(error, error_size, "无法读取 pacman 包详情");
        return false;
    }
    PkgInfo *infos = NULL;
    int info_count = 0;
    parse_si(si, &infos, &info_count);
    NameList installed = parse_names(q, 0);
    NameList updates = parse_names(qu, 0);
    int installed_count = 0;
    InstalledInfo *installed_info = parse_installed(q, &installed_count);

    if (progress && progress->progress)
        progress->progress(progress->ud, "正在整理包列表...", 70, NULL);

    const char *p = sl;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        char *save = NULL;
        char *repo = strtok_r(line, " \t", &save);
        char *name = strtok_r(NULL, " \t", &save);
        char *version = strtok_r(NULL, " \t", &save);
        if (repo && name && version) {
            StorePackage pkg;
            memset(&pkg, 0, sizeof(pkg));
            snprintf(pkg.source, sizeof(pkg.source), "pacman");
            snprintf(pkg.id, sizeof(pkg.id), "%s", name);
            snprintf(pkg.name, sizeof(pkg.name), "%s", name);
            snprintf(pkg.version, sizeof(pkg.version), "%s", version);
            PkgInfo *info = find_info(infos, info_count, name);
            if (info) {
                snprintf(pkg.description, sizeof(pkg.description), "%s", info->description);
                snprintf(pkg.license, sizeof(pkg.license), "%s", info->license);
                snprintf(pkg.category, sizeof(pkg.category), "%s", category_for(info));
                pkg.size_kb = info->size_kb;
            } else {
                snprintf(pkg.category, sizeof(pkg.category), "utility");
            }
            pkg.installed = name_in_list(&installed, name);
            if (pkg.installed) {
                const char *inst = installed_version(installed_info, installed_count, name);
                if (inst) snprintf(pkg.installed_version,
                                   sizeof(pkg.installed_version), "%s", inst);
            }
            pkg.update_available = name_in_list(&updates, name) && !is_protected(name);
            pkg.removable = !is_protected(name);
            find_icon(name, pkg.icon, sizeof(pkg.icon));
            store_list_push(out, &pkg);
        }
        if (!eol) break;
        p = eol + 1;
    }

    free(infos);
    free(installed_info);
    name_list_free(&installed);
    name_list_free(&updates);
    free(sl); free(si); free(q); free(qu);
    if (progress && progress->progress)
        progress->progress(progress->ud, "完成", 100, NULL);
    return true;
}

static bool run_helper(const char *op, const char **ids, int count,
                       StoreProgress *progress, char *error, size_t error_size)
{
    const char *helper = store_helper_path();
    int argc = 4 + count;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    int k = 0;
    if (getenv("TAIJI_STORE_HELPER")) {
        argv[k++] = (char *)helper;
    } else if (command_exists("pkexec")) {
        argv[k++] = "pkexec";
        argv[k++] = (char *)helper;
    } else {
        argv[k++] = (char *)helper;
    }
    argv[k++] = "pacman";
    argv[k++] = (char *)op;
    for (int i = 0; i < count; i++) argv[k++] = (char *)ids[i];
    argv[k] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free(argv);
        if (error) snprintf(error, error_size, "无法创建管道");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); free(argv);
        if (error) snprintf(error, error_size, "无法启动安装进程");
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipefd[1]);
    free(argv);

    char line[512];
    size_t used = 0;
    int status = 0;
    bool cancelled = false;
    for (;;) {
        if (progress && progress->cancel && *progress->cancel) {
            cancelled = true;
            kill(-pid, SIGTERM);
        }
        struct pollfd pfd = {pipefd[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) {
            ssize_t n = read(pipefd[0], line + used, sizeof(line) - used - 1);
            if (n <= 0) break;
            used += (size_t)n;
            line[used] = '\0';
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                *nl = '\0';
                char *msg = line;
                int pct = -1;
                for (char *q = line; *q; q++) {
                    if (isdigit((unsigned char)*q)) {
                        int v = atoi(q);
                        if (v >= 0 && v <= 100 && strchr(q, '%')) { pct = v; break; }
                    }
                }
                if (progress && progress->progress)
                    progress->progress(progress->ud, msg, pct,
                                       msg[0] ? msg : NULL);
                size_t left = strlen(nl + 1);
                memmove(line, nl + 1, left + 1);
                used = left;
            }
            if (used > sizeof(line) - 2) used = 0;
        } else if (pr == 0) {
            int w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                /* drain */
                while (read(pipefd[0], line, sizeof(line)) > 0) {}
                break;
            }
        } else {
            break;
        }
    }
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    if (cancelled) {
        if (error) snprintf(error, error_size, "操作已取消");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error) snprintf(error, error_size, "操作失败（退出码 %d）",
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    if (progress && progress->progress)
        progress->progress(progress->ud, "操作完成", 100, NULL);
    return true;
}

static bool pacman_install(StoreBackend *self, const char **ids, int count,
                           StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    return run_helper("install", ids, count, progress, error, error_size);
}

static bool pacman_remove(StoreBackend *self, const char **ids, int count,
                          StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    return run_helper("remove", ids, count, progress, error, error_size);
}

static bool pacman_update(StoreBackend *self, const char **ids, int count,
                          StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    (void)ids;
    return run_helper("update", NULL, 0, progress, error, error_size);
}

StoreBackend store_pacman_backend = {
    .source = "pacman",
    .label = "系统软件",
    .needs_root = true,
    .supports_remove = true,
    .supports_update = true,
    .probe = pacman_probe,
    .refresh = pacman_refresh,
    .install = pacman_install,
    .remove = pacman_remove,
    .update = pacman_update,
    .is_protected = is_protected,
};
