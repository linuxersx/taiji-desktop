// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "perf.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

const char *perf_mode_label(int mode)
{
    switch (mode) {
        case PERF_MODE_SAVER: return "省电";
        case PERF_MODE_BALANCED: return "均衡";
        case PERF_MODE_PERFORMANCE: return "性能";
        default: return "未知";
    }
}

const char *perf_mode_hint(int mode)
{
    switch (mode) {
        case PERF_MODE_SAVER: return "降低频率和功耗，优先延长续航";
        case PERF_MODE_BALANCED: return "根据负载自动调整，日常推荐";
        case PERF_MODE_PERFORMANCE: return "保持高频，优先流畅度";
        default: return "";
    }
}

int perf_mode_count(const PerfState *state)
{
    if (!state || !state->available) return 0;
    int n = 0;
    for (int i = 0; i < 3; i++) if (state->modes[i]) n++;
    return n;
}

int perf_mode_at(const PerfState *state, int index)
{
    if (!state || index < 0) return PERF_MODE_NONE;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (!state->modes[i]) continue;
        if (n == index) return i;
        n++;
    }
    return PERF_MODE_NONE;
}

void perf_init(PerfState *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = PERF_MODE_NONE;
}

#ifndef __linux__

void perf_poll(PerfState *out)
{
    if (!out) return;
    out->available = false;
    out->mode = PERF_MODE_NONE;

    const char *fake = getenv("TAIJI_TEST_PERF");
    if (fake && *fake) {
        out->available = true;
        out->modes[PERF_MODE_SAVER] = true;
        out->modes[PERF_MODE_BALANCED] = true;
        out->modes[PERF_MODE_PERFORMANCE] = true;
        out->mode = atoi(fake);
        if (out->mode < 0 || out->mode > 2) out->mode = PERF_MODE_BALANCED;
        snprintf(out->backend, sizeof(out->backend), "test");
        snprintf(out->detail, sizeof(out->detail), "CPU governor: schedutil");
    }
}

bool perf_set(PerfState *out, int mode, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    const char *fake = getenv("TAIJI_TEST_PERF");
    if (fake && *fake) {
        if (out) {
            out->mode = mode;
            const char *gov[] = {"powersave", "schedutil", "performance"};
            snprintf(out->detail, sizeof(out->detail), "CPU governor: %s", gov[mode]);
        }
        return true;
    }
    (void)out;
    (void)mode;
    if (error && error_size) snprintf(error, error_size, "性能模式切换仅支持 Linux");
    return false;
}

#else

static bool command_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    char *copy = strdup(path);
    if (!copy) return false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        bool ok = access(full, X_OK) == 0;
        if (ok) { free(copy); return true; }
    }
    free(copy);
    return false;
}

static bool read_line_cmd(const char *cmd, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool got = fgets(out, (int)out_size, f) != NULL;
    pclose(f);
    if (!got) return false;
    out[strcspn(out, "\r\n")] = '\0';
    char *p = out;
    while (*p == ' ' || *p == '\t') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    return out[0] != '\0';
}

static bool read_all_cmd(const char *cmd, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    size_t used = 0;
    while (used + 1 < out_size) {
        size_t n = fread(out + used, 1, out_size - used - 1, f);
        if (n == 0) break;
        used += n;
    }
    out[used] = '\0';
    int rc = pclose(f);
    return rc == 0 && used > 0;
}

static int run_cmd(const char *cmd)
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

static void read_text_file(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        char *p = out;
        while (*p == ' ' || *p == '\t') p++;
        if (p != out) memmove(out, p, strlen(p) + 1);
        out[strcspn(out, "\r\n")] = '\0';
    }
    fclose(f);
}

static bool write_text_file(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", value) > 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool privileged_shell(const char *shell_cmd)
{
    char cmd[2048];
    if (command_exists("sudo")) {
        snprintf(cmd, sizeof(cmd), "sudo -n sh -c '%s' 2>/dev/null", shell_cmd);
        if (run_cmd(cmd) == 0) return true;
    }
    if (command_exists("pkexec")) {
        snprintf(cmd, sizeof(cmd), "pkexec sh -c '%s' 2>/dev/null", shell_cmd);
        if (run_cmd(cmd) == 0) return true;
    }
    return false;
}

static bool privileged_write(const char *path, const char *value)
{
    if (write_text_file(path, value)) return true;
    char shell[1024];
    snprintf(shell, sizeof(shell), "printf \"%%s\\n\" %s > %s", value, path);
    return privileged_shell(shell);
}

static int profile_mode(const char *value)
{
    if (!value || !*value) return PERF_MODE_NONE;
    if (strstr(value, "power-saver") || strstr(value, "low-power") ||
        strstr(value, "quiet") || str_ieq(value, "powersave"))
        return PERF_MODE_SAVER;
    if (strstr(value, "performance")) return PERF_MODE_PERFORMANCE;
    return PERF_MODE_BALANCED;
}

static bool try_ppd(PerfState *out)
{
    if (!command_exists("powerprofilesctl")) return false;

    char list[4096];
    if (!read_all_cmd("powerprofilesctl list 2>/dev/null", list, sizeof(list)))
        return false;

    out->modes[PERF_MODE_SAVER] = strstr(list, "power-saver") != NULL;
    out->modes[PERF_MODE_BALANCED] = strstr(list, "balanced") != NULL;
    out->modes[PERF_MODE_PERFORMANCE] = strstr(list, "performance") != NULL;
    if (!out->modes[PERF_MODE_SAVER] && !out->modes[PERF_MODE_BALANCED] &&
        !out->modes[PERF_MODE_PERFORMANCE])
        return false;

    char cur[64];
    if (!read_line_cmd("powerprofilesctl get 2>/dev/null", cur, sizeof(cur)))
        return false;

    out->available = true;
    out->mode = profile_mode(cur);
    snprintf(out->backend, sizeof(out->backend), "power-profiles-daemon");
    snprintf(out->detail, sizeof(out->detail), "powerprofilesctl / %s", cur);
    return true;
}

static bool try_platform_profile(PerfState *out)
{
    const char *path = "/sys/firmware/acpi/platform_profile";
    char cur[64];
    read_text_file(path, cur, sizeof(cur));
    if (!cur[0]) return false;

    char choices[256];
    read_text_file("/sys/firmware/acpi/platform_profile_choices",
                   choices, sizeof(choices));
    if (!choices[0]) snprintf(choices, sizeof(choices), "low-power balanced performance");

    out->modes[PERF_MODE_SAVER] =
        strstr(choices, "low-power") || strstr(choices, "quiet");
    out->modes[PERF_MODE_BALANCED] = strstr(choices, "balanced") != NULL;
    out->modes[PERF_MODE_PERFORMANCE] = strstr(choices, "performance") != NULL;
    if (!out->modes[PERF_MODE_SAVER] && !out->modes[PERF_MODE_BALANCED] &&
        !out->modes[PERF_MODE_PERFORMANCE])
        return false;

    out->available = true;
    out->mode = profile_mode(cur);
    snprintf(out->backend, sizeof(out->backend), "platform_profile");
    snprintf(out->detail, sizeof(out->detail), "ACPI platform_profile / %s", cur);
    return true;
}

static void cpufreq_governors(char *available, size_t available_size,
                              char *current, size_t current_size)
{
    available[0] = '\0';
    current[0] = '\0';
    read_text_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",
                   available, available_size);
    read_text_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                   current, current_size);
    if (!available[0] || !current[0]) {
        DIR *d = opendir("/sys/devices/system/cpu");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "cpu", 3) != 0 || !isdigit((unsigned char)e->d_name[3]))
                continue;
            char path[512];
            if (!available[0]) {
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/%s/cpufreq/scaling_available_governors",
                         e->d_name);
                read_text_file(path, available, available_size);
            }
            if (!current[0]) {
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/%s/cpufreq/scaling_governor",
                         e->d_name);
                read_text_file(path, current, current_size);
            }
            if (available[0] && current[0]) break;
        }
        closedir(d);
    }
}

static void cpufreq_best_balanced(char *out, size_t out_size,
                                  const char *available)
{
    const char *candidates[] = {"schedutil", "ondemand", "conservative", "interactive", NULL};
    for (int i = 0; candidates[i]; i++) {
        if (strstr(available, candidates[i])) {
            snprintf(out, out_size, "%s", candidates[i]);
            return;
        }
    }
    snprintf(out, out_size, "%s", "powersave");
}

static bool try_cpufreq(PerfState *out)
{
    char govs[512], cur[64];
    cpufreq_governors(govs, sizeof(govs), cur, sizeof(cur));
    if (!govs[0] || !cur[0]) return false;

    out->modes[PERF_MODE_SAVER] = strstr(govs, "powersave") != NULL;
    out->modes[PERF_MODE_BALANCED] =
        strstr(govs, "schedutil") || strstr(govs, "ondemand") ||
        strstr(govs, "conservative") || strstr(govs, "interactive");
    out->modes[PERF_MODE_PERFORMANCE] = strstr(govs, "performance") != NULL;
    if (!out->modes[PERF_MODE_SAVER] && !out->modes[PERF_MODE_BALANCED] &&
        !out->modes[PERF_MODE_PERFORMANCE])
        return false;

    out->available = true;
    out->mode = profile_mode(cur);
    snprintf(out->backend, sizeof(out->backend), "cpufreq");
    snprintf(out->detail, sizeof(out->detail), "CPU governor: %s", cur);
    return true;
}

void perf_poll(PerfState *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = PERF_MODE_NONE;

    const char *fake = getenv("TAIJI_TEST_PERF");
    if (fake && *fake) {
        out->available = true;
        out->modes[PERF_MODE_SAVER] = true;
        out->modes[PERF_MODE_BALANCED] = true;
        out->modes[PERF_MODE_PERFORMANCE] = true;
        out->mode = atoi(fake);
        if (out->mode < 0 || out->mode > 2) out->mode = PERF_MODE_BALANCED;
        snprintf(out->backend, sizeof(out->backend), "test");
        snprintf(out->detail, sizeof(out->detail), "CPU governor: schedutil");
        return;
    }

    if (try_ppd(out)) return;
    if (try_platform_profile(out)) return;
    try_cpufreq(out);
}

static bool cpufreq_set(const char *governor)
{
    bool any = false;
    bool all_ok = true;
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "cpu", 3) != 0 || !isdigit((unsigned char)e->d_name[3]))
            continue;
        char path[512];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", e->d_name);
        if (access(path, F_OK) != 0) continue;
        any = true;
        if (!write_text_file(path, governor)) all_ok = false;
    }
    closedir(d);
    if (!any) return false;
    if (all_ok) return true;

    char shell[512];
    snprintf(shell, sizeof(shell),
             "for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do "
             "[ -e \"$f\" ] && printf \"%%s\\n\" %s > \"$f\" || exit 1; done",
             governor);
    return privileged_shell(shell);
}

bool perf_set(PerfState *out, int mode, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    const char *fake = getenv("TAIJI_TEST_PERF");
    if (fake && *fake) {
        if (out) {
            out->mode = mode;
            const char *gov[] = {"powersave", "schedutil", "performance"};
            snprintf(out->detail, sizeof(out->detail), "CPU governor: %s", gov[mode]);
        }
        return true;
    }
    if (!out || !out->available || mode < 0 || mode > 2) {
        if (error && error_size) snprintf(error, error_size, "当前系统不支持性能模式切换");
        return false;
    }
    if (!out->modes[mode]) {
        if (error && error_size)
            snprintf(error, error_size, "当前系统不支持%s模式", perf_mode_label(mode));
        return false;
    }

    bool ok = false;
    if (strcmp(out->backend, "power-profiles-daemon") == 0) {
        const char *keys[] = {"power-saver", "balanced", "performance"};
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "powerprofilesctl set %s 2>/dev/null", keys[mode]);
        ok = run_cmd(cmd) == 0;
    } else if (strcmp(out->backend, "platform_profile") == 0) {
        char choices[256];
        read_text_file("/sys/firmware/acpi/platform_profile_choices",
                       choices, sizeof(choices));
        const char *value = NULL;
        if (mode == PERF_MODE_SAVER)
            value = strstr(choices, "low-power") ? "low-power" :
                    (strstr(choices, "quiet") ? "quiet" : "low-power");
        else if (mode == PERF_MODE_BALANCED)
            value = "balanced";
        else
            value = "performance";
        ok = privileged_write("/sys/firmware/acpi/platform_profile", value);
    } else {
        char govs[512], cur[64], target[32];
        cpufreq_governors(govs, sizeof(govs), cur, sizeof(cur));
        if (mode == PERF_MODE_SAVER) snprintf(target, sizeof(target), "powersave");
        else if (mode == PERF_MODE_PERFORMANCE) snprintf(target, sizeof(target), "performance");
        else cpufreq_best_balanced(target, sizeof(target), govs);
        ok = cpufreq_set(target);
    }

    if (!ok) {
        if (error && error_size)
            snprintf(error, error_size,
                     "切换失败，需要写入 CPU/平台权限；可安装 power-profiles-daemon 或配置 sudo/udev 规则");
        return false;
    }

    perf_poll(out);
    if (out->mode != mode) {
        if (error && error_size) snprintf(error, error_size, "切换后未能读取到目标模式");
        return false;
    }
    return true;
}

#endif
