// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "system_locale.h"
#include "util.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

static int run_status(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) { /* drain */ }
    int rc = pclose(f);
    if (rc < 0) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 1;
}

static bool read_line(const char *cmd, char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    pclose(f);
    if (!ok) return false;
    out[strcspn(out, "\r\n")] = '\0';
    char *p = out;
    while (*p == ' ' || *p == '\t') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    return out[0] != '\0';
}

static bool privileged_shell(const char *shell_cmd)
{
    char cmd[2048];
    if (command_exists("sudo")) {
        snprintf(cmd, sizeof(cmd), "sudo -n sh -c '%s' >/dev/null 2>&1", shell_cmd);
        if (run_status(cmd) == 0) return true;
    }
    if (command_exists("pkexec")) {
        /* 没有 polkit 代理时不要让认证提示卡住界面。 */
        snprintf(cmd, sizeof(cmd), "timeout 8 pkexec sh -c '%s' >/dev/null 2>&1",
                 shell_cmd);
        if (run_status(cmd) == 0) return true;
    }
    return false;
}

bool system_timezone_get(char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = '\0';

    char link[PATH_MAX];
    ssize_t len = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (len > 0) {
        link[len] = '\0';
        const char *z = strstr(link, "/zoneinfo/");
        if (z) {
            snprintf(out, n, "%s", z + 10);
            return out[0] != '\0';
        }
    }
    FILE *f = fopen("/etc/timezone", "r");
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = '\0';
        fclose(f);
        if (out[0]) return true;
    }
    if (command_exists("timedatectl") &&
        read_line("timedatectl show -p Timezone --value 2>/dev/null", out, n)) {
        return true;
    }
    snprintf(out, n, "UTC");
    return false;
}

bool system_timezone_set(const char *tz, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!tz || !*tz) {
        if (error) snprintf(error, error_size, "时区不能为空");
        return false;
    }
    if (tz[0] == '/' || strstr(tz, "..")) {
        if (error) snprintf(error, error_size, "时区格式无效");
        return false;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", tz);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (error) snprintf(error, error_size, "找不到该时区，请输入如 Asia/Shanghai");
        return false;
    }

    char *q = shell_quote(tz);
    char cmd[PATH_MAX + 64];
    bool ok = false;
    if (command_exists("timedatectl")) {
        snprintf(cmd, sizeof(cmd), "timedatectl set-timezone %s >/dev/null 2>&1", q);
        ok = run_status(cmd) == 0;
        if (!ok) {
            snprintf(cmd, sizeof(cmd), "timedatectl set-timezone %s", q);
            ok = privileged_shell(cmd);
        }
    }
    if (!ok) {
        snprintf(cmd, sizeof(cmd), "ln -sf /usr/share/zoneinfo/%s /etc/localtime", q);
        ok = privileged_shell(cmd);
    }
    free(q);

    if (!ok) {
        if (error) snprintf(error, error_size, "修改时区失败，需要系统权限");
        return false;
    }
    setenv("TZ", tz, 1);
    tzset();
    return true;
}

static const char *language_locale(int lang)
{
    return lang == 1 ? "en_US.UTF-8" : "zh_CN.UTF-8";
}

static bool locale_available(const char *loc)
{
    locale_t l = newlocale(LC_ALL_MASK, loc, (locale_t)0);
    if (!l) return false;
    freelocale(l);
    return true;
}

void system_language_export(int lang)
{
    const char *loc = language_locale(lang);
    setenv("LANG", loc, 1);
    setenv("LC_ALL", loc, 1);
    setenv("LC_MESSAGES", loc, 1);
    setenv("LANGUAGE", lang == 1 ? "en_US:en" : "zh_CN:zh", 1);
    if (!setlocale(LC_ALL, loc)) {
        /* 目标 locale 未生成时保底，避免界面直接回退到纯 C */
        if (!setlocale(LC_ALL, "C.UTF-8"))
            setlocale(LC_ALL, "C");
    }
    tzset();
}

static void notify_user_session(void)
{
    run_status("systemctl --user import-environment "
               "LANG LC_ALL LC_MESSAGES LANGUAGE >/dev/null 2>&1");
    run_status("dbus-update-activation-environment --systemd "
               "LANG LC_ALL LC_MESSAGES LANGUAGE >/dev/null 2>&1");
}

static bool write_user_environment(int lang)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char dir[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/environment.d", xdg);
    else snprintf(dir, sizeof(dir), "%s/.config/environment.d", home);
    mkdir(dir, 0755);
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/90-taiji-locale.conf", dir);
    const char *loc = language_locale(lang);
    char text[256];
    snprintf(text, sizeof(text),
             "LANG=%s\nLC_ALL=%s\nLC_MESSAGES=%s\nLANGUAGE=%s\n",
             loc, loc, loc, lang == 1 ? "en_US:en" : "zh_CN:zh");
    return write_file_atomic(path, text, strlen(text));
}

int system_language_apply(int lang, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (lang != 0 && lang != 1) return 0;
    const char *loc = language_locale(lang);
    if (!locale_available(loc)) {
        if (error && error_size)
            snprintf(error, error_size,
                     "系统缺少 %s，请先执行 sudo locale-gen", loc);
        return 0;
    }
    system_language_export(lang);
    notify_user_session();
    bool user = write_user_environment(lang);

    bool system = false;
    if (command_exists("localectl")) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "localectl set-locale LANG=%s >/dev/null 2>&1",
                 language_locale(lang));
        system = run_status(cmd) == 0;
        if (!system) {
            snprintf(cmd, sizeof(cmd), "localectl set-locale LANG=%s",
                     language_locale(lang));
            system = privileged_shell(cmd);
        }
    }
    if (system) return 2;
    if (user) return 1;
    if (error) snprintf(error, error_size, "无法写入语言环境配置");
    return 0;
}
