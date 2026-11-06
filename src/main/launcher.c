// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher.h"
#include "apps.h"
#include "catalog.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void launcher_init(Launcher *l)
{
    memset(l, 0, sizeof(*l));
    l->pid = -1;
}

void launcher_free(Launcher *l)
{
    free(l->game_id);
    free(l->command);
    memset(l, 0, sizeof(*l));
    l->pid = -1;
}

static char *replace_all(const char *src, const char *needle, const char *value)
{
    if (!src) return xstrdup("");
    if (!needle || !*needle) return xstrdup(src);
    size_t nlen = strlen(needle);
    size_t vlen = strlen(value ? value : "");
    size_t count = 0;
    for (const char *p = src; (p = strstr(p, needle)); p += nlen) count++;
    size_t len = strlen(src) + count * (vlen > nlen ? vlen - nlen : 0) + 1;
    char *out = xmalloc(len);
    char *w = out;
    for (const char *p = src; *p;) {
        const char *hit = strstr(p, needle);
        if (!hit) {
            size_t rest = strlen(p);
            memcpy(w, p, rest);
            w += rest;
            break;
        }
        size_t chunk = (size_t)(hit - p);
        memcpy(w, p, chunk); w += chunk;
        memcpy(w, value ? value : "", vlen); w += vlen;
        p = hit + nlen;
    }
    *w = '\0';
    return out;
}

char *launcher_build_command(const Config *cfg, const Platform *platform, const Game *game)
{
    const char *tmpl = NULL;
    if (game && game->command && *game->command) tmpl = game->command;
    else if (platform && cfg) tmpl = config_command_for(cfg, platform->id);
    char *fallback = NULL;
    if (!tmpl || !*tmpl) {
#ifdef __APPLE__
        fallback = catalog_default_command(platform ? platform->id : NULL, true);
#else
        fallback = catalog_default_command(platform ? platform->id : NULL, false);
#endif
        tmpl = fallback;
    }

    char *rom = shell_quote(game && game->rom_path ? game->rom_path : "");
    char *name = shell_quote(game && game->name ? game->name : "");
    char *plat = shell_quote(platform && platform->name ? platform->name : "");
    char *dir = shell_quote(game && game->rom_path ? "" : "");
    if (game && game->rom_path) {
        free(dir);
        dir = path_dirname_dup(game->rom_path);
        char *q = shell_quote(dir);
        free(dir);
        dir = q;
    }

    char *a = replace_all(tmpl, "%ROM%", rom);
    char *b = replace_all(a, "%NAME%", name);
    char *c = replace_all(b, "%PLATFORM%", plat);
    char *d = replace_all(c, "%DIR%", dir);
    bool has_placeholder = strstr(tmpl, "%ROM%") || strstr(tmpl, "%NAME%") ||
                           strstr(tmpl, "%PLATFORM%") || strstr(tmpl, "%DIR%");
    if (!has_placeholder && game && game->rom_path && *game->rom_path) {
        /* If the template did not include a placeholder, append the ROM path so
         * "retroarch" still launches something useful. */
        char *e = xasprintf("%s %s", d, rom);
        free(d);
        d = e;
    }
    free(a); free(b); free(c); free(rom); free(name); free(plat); free(dir);
    free(fallback);
    return d;
}

bool launcher_start(Launcher *l, const Config *cfg, const Platform *platform,
                    const Game *game, char **error)
{
    if (error) *error = NULL;
    if (l->active) {
        if (error) *error = xstrdup("已有游戏正在运行");
        return false;
    }
    if (!game || !game->rom_path || !file_exists(game->rom_path)) {
        if (error) *error = xasprintf("找不到 ROM 文件：%s", game ? game->rom_path : "");
        return false;
    }
    char *cmd = launcher_build_command(cfg, platform, game);
    fprintf(stderr, "[taiji] launch: %s\n", cmd);

    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = xasprintf("无法启动进程：%s", strerror(errno));
        free(cmd);
        return false;
    }
    if (pid == 0) {
        setsid();
        char *log_path = xasprintf("%s/launch.log", taiji_config_dir());
        int fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        free(log_path);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    l->pid = pid;
    l->active = true;
    l->external = false;
    l->game_session = true;
    l->started_at = (int64_t)time(NULL);
    free(l->game_id);
    l->game_id = xstrdup(game->id);
    free(l->command);
    l->command = cmd;
    return true;
}

static char *session_helper_path(void)
{
    const char *env = getenv("TAIJI_APP_HELPER");
    if (env && *env) return xstrdup(env);
    return xstrdup("/usr/libexec/taiji/taiji-run-app");
}

bool launcher_session_available(void)
{
    const char *mode = getenv("TAIJI_SESSION_MODE");
    if (!mode || !*mode) return false;
    char *helper = session_helper_path();
    bool ok = access(helper, X_OK) == 0;
    free(helper);
    return ok;
}

static void sanitize_child_env(void)
{
    static const char *drop[] = {
        "TAIJI_WINDOW", "TAIJI_SCREENSHOT", "TAIJI_SCREENSHOT_DELAY",
        "TAIJI_START_SCREEN", "TAIJI_START_COLLECTION", "TAIJI_TEST_ACTION",
        "TAIJI_TEST_RESIZE", "TAIJI_TEST_ALLOW_OVERSIZE", "TAIJI_KIOSK",
        "TAIJI_FULLSCREEN", "SDL_RENDER_DRIVER",
    };
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) unsetenv(drop[i]);
}

static bool exe_in_path(const char *name, char *out, size_t out_size)
{
    if (!name || !*name) return false;
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            snprintf(out, out_size, "%s", name);
            return true;
        }
        return false;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    char *copy = xstrdup(path);
    char *save = NULL;
    bool ok = false;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char *full = path_join(dir, name);
        if (access(full, X_OK) == 0) {
            snprintf(out, out_size, "%s", full);
            ok = true;
            free(full);
            break;
        }
        free(full);
    }
    free(copy);
    return ok;
}

static const char *terminal_program(void)
{
    const char *env = getenv("TAIJI_TERMINAL");
    if (env && *env) return env;
    env = getenv("TERMINAL");
    if (env && *env) return env;
    static char found[PATH_MAX];
    static const char *terms[] = {
        "xdg-terminal-exec", "konsole", "x-terminal-emulator", "foot", "xterm"
    };
    for (size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); i++)
        if (exe_in_path(terms[i], found, sizeof(found))) return found;
    return NULL;
}

static char *terminal_wrap(const char *command, char **error)
{
    const char *term = terminal_program();
    if (!term) {
        if (error) *error = xstrdup("未找到终端程序（xdg-terminal-exec/konsole/xterm）");
        return NULL;
    }
    char *q = shell_quote(command);
    char *out;
    const char *base = strrchr(term, '/');
    base = base ? base + 1 : term;
    if (strcmp(base, "xdg-terminal-exec") == 0)
        out = xasprintf("%s sh -c %s", term, q);
    else
        out = xasprintf("%s -e sh -c %s", term, q);
    free(q);
    return out;
}

static bool command_is_tui(const char *command)
{
    static const char *known[] = {
        "htop", "btop", "top", "nano", "vim", "nvim", "vi", "mc", "ranger",
        "yazi", "nnn", "tmux", "screen", "ncdu", "duf", "cmus", "ncmpcpp",
        "alsamixer", "pacman", "yay", "paru", "neofetch", "fastfetch",
        "less", "more", "man", "watch", "iftop", "nmtui", "nethack"
    };
    if (!command || !*command) return false;
    char tok[PATH_MAX];
    size_t k = 0;
    const char *p = command;
    while (*p == ' ' || *p == '\t') p++;
    for (; *p && *p != ' ' && *p != '\t' && k + 1 < sizeof(tok); p++)
        tok[k++] = *p;
    tok[k] = '\0';
    if (!tok[0]) return false;
    const char *bn = strrchr(tok, '/');
    bn = bn ? bn + 1 : tok;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(bn, known[i]) == 0) return true;

    char exe[PATH_MAX];
    if (!exe_in_path(tok, exe, sizeof(exe))) return false;
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "ldd %s 2>/dev/null", exe);
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    char buf[512];
    bool tui = false;
    bool gui = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "libncurses") || strstr(buf, "libtinfo"))
            tui = true;
        if (strstr(buf, "libQt6") || strstr(buf, "libQt5") ||
            strstr(buf, "libgtk") || strstr(buf, "libgdk") ||
            strstr(buf, "libSDL") || strstr(buf, "libwayland") ||
            strstr(buf, "libX11") || strstr(buf, "libxcb") ||
            strstr(buf, "libGL") || strstr(buf, "libEGL") ||
            strstr(buf, "libvulkan"))
            gui = true;
    }
    pclose(f);
    /* LLVM/curl-style dependencies can pull in libncurses without the app
     * being a terminal UI.  A GUI toolkit or display library wins. */
    return tui && !gui;
}

static bool entry_needs_terminal(const AppEntry *entry)
{
    if (entry->terminal == APP_TERMINAL_YES) return true;
    if (entry->terminal == APP_TERMINAL_NO) return false;
    return command_is_tui(entry->command);
}

bool launcher_entry_needs_terminal(const AppEntry *entry)
{
    return entry_needs_terminal(entry);
}

bool launcher_start_app(Launcher *l, const AppEntry *entry, char **error)
{
    if (error) *error = NULL;
    if (l->active) {
        if (error) *error = xstrdup("已有程序正在运行");
        return false;
    }
    if (!entry || !entry->command[0]) {
        if (error) *error = xstrdup("应用没有配置启动命令");
        return false;
    }

    char *run_command = xstrdup(entry->command);
    if (entry_needs_terminal(entry)) {
        char *wrapped = terminal_wrap(entry->command, error);
        if (!wrapped) {
            free(run_command);
            return false;
        }
        free(run_command);
        run_command = wrapped;
    }

    char *cmd = NULL;
    bool use_helper = launcher_session_available();
    if (use_helper) {
        char *helper = session_helper_path();
        char *q_id = shell_quote(entry->id);
        char *q_mode = shell_quote(entry->background ? "background"
                                                      : apps_mode_name(entry->mode));
        char *q_ret = shell_quote(apps_return_name(entry->on_return));
        char *q_cmd = shell_quote(run_command);
        cmd = xasprintf("%s %s %s %s %s", helper, q_id, q_mode, q_ret, q_cmd);
        free(helper); free(q_id); free(q_mode); free(q_ret); free(q_cmd);
    } else {
        cmd = xstrdup(run_command);
    }
    free(run_command);

    fprintf(stderr, "[taiji] launch app: %s (%s)\n", entry->name, cmd);
    bool detach = use_helper && strncmp(entry->id, "game:", 5) != 0;

    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = xasprintf("无法启动进程：%s", strerror(errno));
        free(cmd);
        return false;
    }
    if (pid == 0) {
        setsid();
        if (detach) {
            pid_t grand = fork();
            if (grand > 0) _exit(0);
        }
        sanitize_child_env();
        if (entry->name[0]) setenv("TAIJI_APP_NAME", entry->name, 1);
        char *log_path = apps_log_path(entry->id);
        int fd = open(log_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        free(log_path);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (detach) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        free(cmd);
        return true;
    }
    l->pid = pid;
    l->active = true;
    l->external = true;
    l->game_session = false;
    l->started_at = (int64_t)time(NULL);
    free(l->game_id);
    l->game_id = xstrdup(entry->id);
    free(l->command);
    l->command = cmd;
    return true;
}

int launcher_poll(Launcher *l, int64_t *elapsed_seconds)
{
    if (!l->active || l->pid <= 0) return -1;
    int status = 0;
    pid_t r = waitpid(l->pid, &status, WNOHANG);
    if (r == 0) return 0;
    if (r < 0 && errno == EINTR) return 0;
    int64_t elapsed = (int64_t)time(NULL) - l->started_at;
    if (elapsed < 0) elapsed = 0;
    if (elapsed_seconds) *elapsed_seconds = elapsed;
    l->active = false;
    l->external = false;
    l->game_session = false;
    l->pid = -1;
    return 1;
}
