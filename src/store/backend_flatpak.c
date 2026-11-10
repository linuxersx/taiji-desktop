// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "backend_flatpak.h"
#include "flatpak_presets.h"
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

#define FLATPAK_REMOTE "flathub"

typedef struct {
    char id[STORE_ID_MAX];
    char version[64];
} FlatpakInstalled;

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

static bool contains_icase(const char *text, const char *needle)
{
    return text && needle && *needle && strcasestr(text, needle) != NULL;
}

static bool flatpak_network_error(const char *text)
{
    return contains_icase(text, "Could not resolve hostname") ||
           contains_icase(text, "Temporary failure in name resolution") ||
           contains_icase(text, "Name or service not known") ||
           contains_icase(text, "Network is down") ||
           contains_icase(text, "No route to host") ||
           contains_icase(text, "Network unreachable") ||
           contains_icase(text, "Failed to connect") ||
           contains_icase(text, "Could not connect") ||
           contains_icase(text, "Connection timed out") ||
           contains_icase(text, "Operation timed out") ||
           contains_icase(text, "While fetching") ||
           contains_icase(text, "Unable to load summary");
}

static void flatpak_copy_last_line(const char *text, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!text || !*text) return;
    const char *last = NULL;
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            if (*p) last = p;
            break;
        }
        if (nl > p) last = p;
        p = nl + 1;
    }
    if (!last) return;
    size_t len = strcspn(last, "\r\n");
    if (len >= out_size) len = out_size - 1;
    memcpy(out, last, len);
    out[len] = '\0';
}

static void flatpak_failure_message(const char *output, char *error,
                                    size_t error_size)
{
    if (!error || error_size == 0) return;
    if (flatpak_network_error(output)) {
        snprintf(error, error_size,
                 "网络不可用：无法连接 Flatpak 软件源，请检查 Wi-Fi、DNS 或网络设置。");
        return;
    }
    if (contains_icase(output, "not authorized") ||
        contains_icase(output, "Authentication") ||
        contains_icase(output, "polkit") ||
        contains_icase(output, "No authentication agent")) {
        snprintf(error, error_size,
                 "没有权限执行 Flatpak 操作，请检查系统授权服务（polkit）。");
        return;
    }
    if (contains_icase(output, "No space left on device") ||
        contains_icase(output, "disk full") ||
        contains_icase(output, "not enough space")) {
        snprintf(error, error_size,
                 "磁盘空间不足，无法完成 Flatpak 操作。");
        return;
    }
    if (contains_icase(output, "No such ref") ||
        contains_icase(output, "not installed") ||
        contains_icase(output, "not found") ||
        contains_icase(output, "No remote refs")) {
        snprintf(error, error_size,
                 "找不到该 Flatpak 应用或软件源，请刷新商店后重试。");
        return;
    }

    char detail[256];
    flatpak_copy_last_line(output, detail, sizeof(detail));
    if (detail[0])
        snprintf(error, error_size, "Flatpak 操作失败：%.200s", detail);
    else
        snprintf(error, error_size,
                 "Flatpak 操作失败，请检查网络、权限或系统日志。");
}

static long long parse_size_kb(const char *s)
{
    double v = 0;
    char unit[24] = "";
    if (!s || !*s) return 0;
    if (sscanf(s, "%lf %23s", &v, unit) < 1) return 0;
    if (strncasecmp(unit, "GiB", 3) == 0 || strncasecmp(unit, "GB", 2) == 0) v *= 1024 * 1024;
    else if (strncasecmp(unit, "MiB", 3) == 0 || strncasecmp(unit, "MB", 2) == 0) v *= 1024;
    else if (strncasecmp(unit, "KiB", 3) == 0 || strncasecmp(unit, "KB", 2) == 0) { }
    else if (strncasecmp(unit, "B", 1) == 0) v /= 1024;
    return (long long)v;
}

static FlatpakInstalled *parse_installed(int *count)
{
    *count = 0;
    char *text = run_capture("flatpak list --system --app --columns=application,version 2>/dev/null");
    if (!text) return NULL;
    int cap = 0;
    FlatpakInstalled *out = NULL;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (*count == cap) {
            cap = cap ? cap * 2 : 64;
            out = realloc(out, (size_t)cap * sizeof(*out));
        }
        snprintf(out[*count].id, sizeof(out[*count].id), "%s", line);
        snprintf(out[*count].version, sizeof(out[*count].version), "%s", tab + 1);
        (*count)++;
    }
    free(text);
    return out;
}

static const char *installed_version(FlatpakInstalled *list, int count, const char *id)
{
    for (int i = 0; i < count; i++)
        if (strcmp(list[i].id, id) == 0) return list[i].version;
    return NULL;
}

static const char *flatpak_category(const char *id, const char *summary)
{
    if (strcasestr(id, "game") || strcasestr(summary, "game") ||
        strcasestr(summary, "游戏")) return "games";
    if (strcasestr(id, "code") || strcasestr(id, "studio") ||
        strcasestr(id, "ide") || strcasestr(summary, "develop") ||
        strcasestr(summary, "开发")) return "development";
    if (strcasestr(id, "player") || strcasestr(id, "video") ||
        strcasestr(id, "audio") || strcasestr(summary, "video") ||
        strcasestr(summary, "music") || strcasestr(summary, "音")) return "media";
    if (strcasestr(id, "browser") || strcasestr(id, "chat") ||
        strcasestr(id, "mail") || strcasestr(summary, "network") ||
        strcasestr(summary, "web")) return "network";
    if (strcasestr(id, "system") || strcasestr(summary, "system") ||
        strcasestr(summary, "系统")) return "system";
    if (strcasestr(id, "desktop") || strcasestr(summary, "desktop"))
        return "desktop";
    return "utility";
}

static void find_icon(const char *id, char *out, size_t n)
{
    out[0] = '\0';
    static const char *sizes[] = {"scalable", "512x512", "256x256", "128x128", "64x64", "48x48"};
    static const char *exts[] = {".png", ".svg"};
    static const char *roots[] = {
        "/var/lib/flatpak/exports/share/icons/hicolor",
        "/var/lib/flatpak/app/%s/current/active/export/share/icons/hicolor",
    };
    char user[PATH_MAX] = "";
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(user, sizeof(user), "%s/.local/share/flatpak/exports/share/icons/hicolor", home);
    for (int r = 0; r < 3 && !out[0]; r++) {
        char base[PATH_MAX];
        if (r < 2) snprintf(base, sizeof(base), "%s", roots[r]);
        else snprintf(base, sizeof(base), "%s", user);
        if (r == 1) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), roots[1], id);
            snprintf(base, sizeof(base), "%s", tmp);
        }
        if (!base[0]) continue;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]) && !out[0]; s++) {
            for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]) && !out[0]; e++) {
                char p[PATH_MAX];
                snprintf(p, sizeof(p), "%s/%s/apps/%s%s", base, sizes[s], id, exts[e]);
                if (access(p, R_OK) == 0) snprintf(out, n, "%s", p);
            }
        }
    }
}

int store_flatpak_mirror_count(void) { return TAIJI_FLATPAK_MIRROR_COUNT; }

const char *store_flatpak_mirror_label(int index)
{
    if (index < 0 || index >= TAIJI_FLATPAK_MIRROR_COUNT) return NULL;
    return taiji_flatpak_mirrors[index].label;
}

const char *store_flatpak_mirror_url(int index)
{
    if (index < 0 || index >= TAIJI_FLATPAK_MIRROR_COUNT) return NULL;
    return taiji_flatpak_mirrors[index].url;
}

const char *store_flatpak_mirror_id(int index)
{
    if (index < 0 || index >= TAIJI_FLATPAK_MIRROR_COUNT) return NULL;
    return taiji_flatpak_mirrors[index].id;
}

static const char *host_of(const char *url)
{
    const char *p = url ? strstr(url, "://") : NULL;
    return p ? p + 3 : url;
}

int store_flatpak_mirror_current(void)
{
    char *text = run_capture("flatpak remotes --system --columns=name,url 2>/dev/null");
    if (!text) return -1;
    int found = -1;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        const char *url = tab + 1;
        const char *hp = host_of(url);
        for (int i = 0; i < TAIJI_FLATPAK_MIRROR_COUNT; i++) {
            const char *want = host_of(taiji_flatpak_mirrors[i].url);
            if (strncmp(hp, want, strlen(want)) == 0) {
                found = i;
                goto done;
            }
        }
    }
done:
    free(text);
    return found;
}

static bool flatpak_probe(char *error, size_t error_size)
{
    if (!command_exists("flatpak")) {
        if (error) snprintf(error, error_size, "未找到 flatpak");
        return false;
    }
    if (access(store_helper_path(), X_OK) != 0 &&
        getenv("TAIJI_STORE_HELPER") == NULL) {
        if (error) snprintf(error, error_size, "未找到商店 helper");
        return false;
    }
    return true;
}

static bool flatpak_refresh(StoreBackend *self, StorePackageList *out,
                            StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    if (progress && progress->progress)
        progress->progress(progress->ud, "正在读取 Flatpak 仓库...", 20, NULL);
    char *apps = run_capture("flatpak remote-ls --system --app --columns=application,name,version,description,installed-size,origin 2>&1");
    bool basic_cols = false;
    if (!apps || !*apps)
        apps = run_capture("flatpak remote-ls --system --app --columns=application,name,version,origin 2>&1");
    if (!apps) {
        if (error) snprintf(error, error_size,
                            "无法读取 Flatpak 软件源，请检查网络或软件源配置。");
        return false;
    }
    char err_line[256] = "";
    const char *lf = strchr(apps, '\n');
    size_t elen = lf ? (size_t)(lf - apps) : strlen(apps);
    if (elen >= sizeof(err_line)) elen = sizeof(err_line) - 1;
    memcpy(err_line, apps, elen);
    err_line[elen] = '\0';
    if (!strchr(apps, '\t')) {
        free(apps);
        apps = run_capture("flatpak remote-ls --system --app --columns=application,name,version,origin 2>&1");
        basic_cols = true;
        if (!apps) {
            if (flatpak_network_error(err_line))
                flatpak_failure_message(err_line, error, error_size);
            else if (error)
                snprintf(error, error_size, "%s",
                         err_line[0] ? err_line : "Flatpak 仓库读取失败");
            return false;
        }
        lf = strchr(apps, '\n');
        elen = lf ? (size_t)(lf - apps) : strlen(apps);
        if (elen >= sizeof(err_line)) elen = sizeof(err_line) - 1;
        memcpy(err_line, apps, elen);
        err_line[elen] = '\0';
    }

    int installed_count = 0;
    FlatpakInstalled *installed = parse_installed(&installed_count);
    if (progress && progress->progress)
        progress->progress(progress->ud, "正在整理 Flatpak 应用...", 70, NULL);

    char *save = NULL;
    for (char *line = strtok_r(apps, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        bool has_tab = strchr(line, '\t') != NULL;
        char *fields[6] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 6) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = '\0';
            p = tab + 1;
        }
        if (!has_tab || nf < 2 || !fields[0][0]) continue;
        StorePackage pkg;
        memset(&pkg, 0, sizeof(pkg));
        snprintf(pkg.source, sizeof(pkg.source), "flatpak");
        snprintf(pkg.id, sizeof(pkg.id), "%s", fields[0]);
        snprintf(pkg.name, sizeof(pkg.name), "%s", nf >= 2 && fields[1][0] ? fields[1] : fields[0]);
        snprintf(pkg.version, sizeof(pkg.version), "%s", nf >= 3 ? fields[2] : "");
        snprintf(pkg.description, sizeof(pkg.description), "%s",
                 !basic_cols && nf >= 4 ? fields[3] : "");
        pkg.size_kb = !basic_cols && nf >= 5 ? parse_size_kb(fields[4]) : 0;
        snprintf(pkg.category, sizeof(pkg.category), "%s",
                 flatpak_category(pkg.id, pkg.description));
        const char *iv = installed_version(installed, installed_count, pkg.id);
        pkg.installed = iv != NULL;
        if (iv) snprintf(pkg.installed_version, sizeof(pkg.installed_version), "%s", iv);
        pkg.update_available = pkg.installed && pkg.version[0] && iv &&
                               strcmp(pkg.version, iv) != 0;
        pkg.removable = true;
        find_icon(pkg.id, pkg.icon, sizeof(pkg.icon));
        store_list_push(out, &pkg);
    }
    if (out->count == 0) {
        if (flatpak_network_error(apps))
            flatpak_failure_message(apps, error, error_size);
        else if (error && error_size)
            snprintf(error, error_size,
                     "Flatpak 仓库为空或未配置软件源");
        free(installed);
        free(apps);
        return false;
    }
    free(installed);
    free(apps);
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
    argv[k++] = "flatpak";
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
        if (error) snprintf(error, error_size, "无法启动 Flatpak 进程");
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
    char errbuf[2048] = "";
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
                if (line[0]) {
                    size_t cur = strlen(errbuf);
                    size_t add = strlen(line);
                    if (cur + add + 2 >= sizeof(errbuf)) {
                        errbuf[0] = '\0';
                        cur = 0;
                    }
                    if (cur) errbuf[cur++] = ' ';
                    snprintf(errbuf + cur, sizeof(errbuf) - cur, "%s", line);
                }
                if (progress && progress->progress)
                    progress->progress(progress->ud, line, -1,
                                       line[0] ? line : NULL);
                size_t left = strlen(nl + 1);
                memmove(line, nl + 1, left + 1);
                used = left;
            }
            if (used > sizeof(line) - 2) used = 0;
        } else if (pr == 0) {
            int w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                while (read(pipefd[0], line, sizeof(line)) > 0) {}
                break;
            }
        } else break;
    }
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    if (cancelled) {
        if (error) snprintf(error, error_size, "操作已取消");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        flatpak_failure_message(errbuf, error, error_size);
        return false;
    }
    return true;
}

bool store_flatpak_mirror_apply(int index, StoreProgress *progress,
                                char *error, size_t error_size)
{
    if (index < 0 || index >= TAIJI_FLATPAK_MIRROR_COUNT) {
        if (error) snprintf(error, error_size, "未知的 Flatpak 软件源");
        return false;
    }
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "%s", taiji_flatpak_mirrors[index].id);
    const char *ids[] = {idbuf};
    return run_helper("remote", ids, 1, progress, error, error_size);
}

static bool flatpak_install(StoreBackend *self, const char **ids, int count,
                            StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    return run_helper("install", ids, count, progress, error, error_size);
}

static bool flatpak_remove(StoreBackend *self, const char **ids, int count,
                           StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    return run_helper("remove", ids, count, progress, error, error_size);
}

static bool flatpak_update(StoreBackend *self, const char **ids, int count,
                           StoreProgress *progress, char *error, size_t error_size)
{
    (void)self;
    (void)ids;
    (void)count;
    return run_helper("update", NULL, 0, progress, error, error_size);
}

static bool flatpak_is_protected(const char *id)
{
    (void)id;
    return false;
}

StoreBackend store_flatpak_backend = {
    .source = "flatpak",
    .label = "Flatpak",
    .needs_root = true,
    .supports_remove = true,
    .supports_update = true,
    .probe = flatpak_probe,
    .refresh = flatpak_refresh,
    .install = flatpak_install,
    .remove = flatpak_remove,
    .update = flatpak_update,
    .is_protected = flatpak_is_protected,
};
