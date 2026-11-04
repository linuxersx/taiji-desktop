// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "gpu.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

const char *gpu_mode_label(int mode)
{
    switch (mode) {
        case GPU_MODE_AUTO: return "跟随";
        case GPU_MODE_SAVER: return "省电";
        case GPU_MODE_PERFORMANCE: return "性能";
        default: return "未知";
    }
}

const char *gpu_mode_hint(int mode)
{
    switch (mode) {
        case GPU_MODE_AUTO: return "由驱动和温控自动调节";
        case GPU_MODE_SAVER: return "限制 GPU 频率，降低发热和功耗";
        case GPU_MODE_PERFORMANCE: return "优先保证 GPU 频率和帧率";
        default: return "";
    }
}

int gpu_mode_count(const GpuState *state)
{
    if (!state || !state->available) return 0;
    int n = 0;
    for (int i = 0; i < 3; i++) if (state->modes[i]) n++;
    return n;
}

int gpu_mode_at(const GpuState *state, int index)
{
    if (!state || index < 0) return GPU_MODE_NONE;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (!state->modes[i]) continue;
        if (n == index) return i;
        n++;
    }
    return GPU_MODE_NONE;
}

void gpu_init(GpuState *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = GPU_MODE_NONE;
    out->orig_min_level = -1;
    out->orig_max_level = -1;
}

#ifndef __linux__

void gpu_poll(GpuState *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mode = GPU_MODE_NONE;
    out->orig_min_level = -1;
    out->orig_max_level = -1;

    const char *fake = getenv("TAIJI_TEST_GPU");
    if (fake && *fake) {
        out->available = true;
        out->modes[GPU_MODE_AUTO] = true;
        out->modes[GPU_MODE_SAVER] = true;
        out->modes[GPU_MODE_PERFORMANCE] = true;
        out->mode = atoi(fake);
        if (out->mode < 0 || out->mode > 2) out->mode = GPU_MODE_AUTO;
        snprintf(out->name, sizeof(out->name), "Mali-G31");
        snprintf(out->backend, sizeof(out->backend), "devfreq");
        snprintf(out->detail, sizeof(out->detail), "devfreq / simple_ondemand");
    }
}

bool gpu_set(GpuState *out, int mode, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    const char *fake = getenv("TAIJI_TEST_GPU");
    if (fake && *fake) {
        if (out) {
            out->mode = mode;
            const char *gov[] = {"simple_ondemand", "powersave", "performance"};
            snprintf(out->detail, sizeof(out->detail), "devfreq / %s", gov[mode]);
        }
        return true;
    }
    (void)out;
    (void)mode;
    if (error && error_size) snprintf(error, error_size, "GPU 性能模式仅支持 Linux");
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

static int read_int_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool write_text_file(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", value) > 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool privileged_write(const char *path, const char *value)
{
    if (write_text_file(path, value)) return true;
    char shell[1024];
    snprintf(shell, sizeof(shell), "printf \"%%s\\n\" %s > %s", value, path);
    return privileged_shell(shell);
}

static bool contains_word(const char *list, const char *word)
{
    return list && *list && word && *word && strstr(list, word) != NULL;
}

static int mode_from_governor(const char *gov)
{
    if (!gov || !*gov) return GPU_MODE_NONE;
    if (str_ieq(gov, "performance")) return GPU_MODE_PERFORMANCE;
    if (str_ieq(gov, "powersave")) return GPU_MODE_SAVER;
    return GPU_MODE_AUTO;
}

static const char *gpu_display_name(const char *raw)
{
    if (!raw || !*raw) return "GPU";
    char lower[128];
    size_t n = strlen(raw);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)raw[i]);
    lower[n] = '\0';
    if (strstr(lower, "kgsl") || strstr(lower, "adreno") || strstr(lower, "3d"))
        return "Adreno GPU";
    if (strstr(lower, "mali")) return "Mali GPU";
    if (strstr(lower, "v3d") || strstr(lower, "videocore")) return "VideoCore GPU";
    if (strstr(lower, "gpu")) return "GPU";
    return "GPU";
}

static bool gpu_devfreq_candidate(const char *name)
{
    char lower[128];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[n] = '\0';
    return strstr(lower, "gpu") || strstr(lower, "mali") ||
           strstr(lower, "kgsl") || strstr(lower, "adreno") ||
           strstr(lower, "v3d") || strstr(lower, "3d");
}

static bool try_devfreq(GpuState *out)
{
    DIR *d = opendir("/sys/class/devfreq");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char base[384];
        snprintf(base, sizeof(base), "/sys/class/devfreq/%s", e->d_name);

        char name_path[512], name[128];
        snprintf(name_path, sizeof(name_path), "%s/name", base);
        read_text_file(name_path, name, sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "%s", e->d_name);
        if (!gpu_devfreq_candidate(name)) continue;

        char compat_path[512], compat[192];
        snprintf(compat_path, sizeof(compat_path), "%s/device/of_node/compatible", base);
        read_text_file(compat_path, compat, sizeof(compat));
        const char *display_name = "GPU";
        if (compat[0]) {
            display_name = gpu_display_name(compat);
            if (strcmp(display_name, "GPU") == 0)
                display_name = gpu_display_name(name);
        } else {
            display_name = gpu_display_name(name);
        }

        char gov_path[512], govs_path[512], gov[64], govs[512];
        snprintf(gov_path, sizeof(gov_path), "%s/governor", base);
        snprintf(govs_path, sizeof(govs_path), "%s/available_governors", base);
        read_text_file(gov_path, gov, sizeof(gov));
        read_text_file(govs_path, govs, sizeof(govs));
        if (!gov[0] || !govs[0]) continue;

        bool has_saver = contains_word(govs, "powersave");
        bool has_perf = contains_word(govs, "performance");
        if (!has_saver && !has_perf) continue;

        memset(out, 0, sizeof(*out));
        out->orig_min_level = -1;
        out->orig_max_level = -1;
        out->available = true;
        out->modes[GPU_MODE_AUTO] = true;
        out->modes[GPU_MODE_SAVER] = has_saver;
        out->modes[GPU_MODE_PERFORMANCE] = has_perf;
        out->mode = mode_from_governor(gov);
        snprintf(out->name, sizeof(out->name), "%s", display_name);
        snprintf(out->backend, sizeof(out->backend), "devfreq");
        snprintf(out->governor_path, sizeof(out->governor_path), "%s", gov_path);
        snprintf(out->devfreq_dir, sizeof(out->devfreq_dir), "%s", base);
        snprintf(out->original_governor, sizeof(out->original_governor), "%s", gov);
        snprintf(out->saver_governor, sizeof(out->saver_governor), "%s",
                 has_saver ? "powersave" : "");
        snprintf(out->perf_governor, sizeof(out->perf_governor), "%s",
                 has_perf ? "performance" : "");

        const char *auto_gov = NULL;
        if (gov[0] && !str_ieq(gov, "performance") && !str_ieq(gov, "powersave"))
            auto_gov = gov;
        else if (contains_word(govs, "msm-adreno-tz"))
            auto_gov = "msm-adreno-tz";
        else if (contains_word(govs, "simple_ondemand"))
            auto_gov = "simple_ondemand";
        else if (contains_word(govs, "ondemand"))
            auto_gov = "ondemand";
        else
            auto_gov = gov;
        snprintf(out->auto_governor, sizeof(out->auto_governor), "%s",
                 auto_gov ? auto_gov : "");

        snprintf(out->detail, sizeof(out->detail), "devfreq / %s", gov);
        closedir(d);
        return true;
    }
    closedir(d);
    return false;
}

static int count_ints(const char *text)
{
    int n = 0;
    const char *p = text;
    while (*p) {
        while (*p && !isdigit((unsigned char)*p) && *p != '-') p++;
        if (!*p) break;
        char *end = NULL;
        strtol(p, &end, 10);
        if (end == p) break;
        n++;
        p = end;
    }
    return n;
}

static bool try_kgsl(GpuState *out)
{
    const char *base = "/sys/class/kgsl/kgsl-3d0";
    char min_path[512], max_path[512], freq_path[512], freq[1024];
    snprintf(min_path, sizeof(min_path), "%s/min_pwrlevel", base);
    snprintf(max_path, sizeof(max_path), "%s/max_pwrlevel", base);
    snprintf(freq_path, sizeof(freq_path), "%s/gpu_available_frequencies", base);
    int min_level = read_int_file(min_path);
    int max_level_file = read_int_file(max_path);
    read_text_file(freq_path, freq, sizeof(freq));
    int levels = count_ints(freq);
    if (min_level < 0 || max_level_file < 0 || levels <= 1) return false;

    memset(out, 0, sizeof(*out));
    out->available = true;
    out->modes[GPU_MODE_AUTO] = true;
    out->modes[GPU_MODE_SAVER] = true;
    out->modes[GPU_MODE_PERFORMANCE] = true;
    out->max_level = levels - 1;
    if (min_level == 0 && max_level_file == 0) {
        out->mode = GPU_MODE_PERFORMANCE;
        out->orig_min_level = 0;
        out->orig_max_level = out->max_level;
    } else if (max_level_file >= out->max_level && min_level >= out->max_level) {
        out->mode = GPU_MODE_SAVER;
        out->orig_min_level = 0;
        out->orig_max_level = out->max_level;
    } else {
        out->mode = GPU_MODE_AUTO;
        out->orig_min_level = min_level;
        out->orig_max_level = max_level_file;
    }
    snprintf(out->name, sizeof(out->name), "Adreno GPU");
    snprintf(out->backend, sizeof(out->backend), "kgsl");
    snprintf(out->min_level_path, sizeof(out->min_level_path), "%s", min_path);
    snprintf(out->max_level_path, sizeof(out->max_level_path), "%s", max_path);
    snprintf(out->detail, sizeof(out->detail), "KGSL / pwrlevel");
    return true;
}

static bool try_amdgpu(GpuState *out)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) != 0 || !isdigit((unsigned char)e->d_name[4]))
            continue;
        char base[512], level_path[512], vendor_path[512], vendor[32];
        snprintf(base, sizeof(base), "/sys/class/drm/%s/device", e->d_name);
        snprintf(level_path, sizeof(level_path), "%s/power_dpm_force_performance_level", base);
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", base);
        read_text_file(vendor_path, vendor, sizeof(vendor));
        if (!str_ieq(vendor, "0x1002")) continue;
        char level[64];
        read_text_file(level_path, level, sizeof(level));
        if (!level[0]) continue;

        memset(out, 0, sizeof(*out));
        out->available = true;
        out->modes[GPU_MODE_AUTO] = true;
        out->modes[GPU_MODE_SAVER] = true;
        out->modes[GPU_MODE_PERFORMANCE] = true;
        if (str_ieq(level, "low")) out->mode = GPU_MODE_SAVER;
        else if (str_ieq(level, "high")) out->mode = GPU_MODE_PERFORMANCE;
        else out->mode = GPU_MODE_AUTO;
        out->orig_min_level = -1;
        out->orig_max_level = -1;
        snprintf(out->name, sizeof(out->name), "AMD Radeon");
        snprintf(out->backend, sizeof(out->backend), "amdgpu");
        snprintf(out->governor_path, sizeof(out->governor_path), "%s", level_path);
        snprintf(out->detail, sizeof(out->detail), "amdgpu / %s", level);
        closedir(d);
        return true;
    }
    closedir(d);
    return false;
}

void gpu_poll(GpuState *out)
{
    if (!out) return;
    GpuState prev = *out;

    memset(out, 0, sizeof(*out));
    out->mode = GPU_MODE_NONE;
    out->orig_min_level = -1;
    out->orig_max_level = -1;

    const char *fake = getenv("TAIJI_TEST_GPU");
    if (fake && *fake) {
        out->available = true;
        out->modes[GPU_MODE_AUTO] = true;
        out->modes[GPU_MODE_SAVER] = true;
        out->modes[GPU_MODE_PERFORMANCE] = true;
        out->mode = atoi(fake);
        if (out->mode < 0 || out->mode > 2) out->mode = GPU_MODE_AUTO;
        snprintf(out->name, sizeof(out->name), "Mali-G31");
        snprintf(out->backend, sizeof(out->backend), "devfreq");
        snprintf(out->detail, sizeof(out->detail), "devfreq / simple_ondemand");
        return;
    }

    bool found = try_devfreq(out) || try_kgsl(out) || try_amdgpu(out);
    if (found && prev.available && strcmp(prev.backend, out->backend) == 0 &&
        strcmp(prev.governor_path, out->governor_path) == 0 &&
        strcmp(prev.min_level_path, out->min_level_path) == 0) {
        if (strcmp(out->backend, "devfreq") == 0) {
            snprintf(out->original_governor, sizeof(out->original_governor), "%s",
                     prev.original_governor);
            snprintf(out->auto_governor, sizeof(out->auto_governor), "%s",
                     prev.auto_governor);
            snprintf(out->saver_governor, sizeof(out->saver_governor), "%s",
                     prev.saver_governor);
            snprintf(out->perf_governor, sizeof(out->perf_governor), "%s",
                     prev.perf_governor);
        } else if (strcmp(out->backend, "kgsl") == 0) {
            out->orig_min_level = prev.orig_min_level;
            out->orig_max_level = prev.orig_max_level;
            out->max_level = prev.max_level;
        }
    }
}

static bool verify_devfreq(GpuState *out, int mode)
{
    char gov[64];
    read_text_file(out->governor_path, gov, sizeof(gov));
    int actual = mode_from_governor(gov);
    if (mode == GPU_MODE_AUTO) {
        if (actual == GPU_MODE_AUTO) {
            snprintf(out->detail, sizeof(out->detail), "devfreq / %s", gov);
            return true;
        }
        return false;
    }
    if (actual != mode) return false;
    snprintf(out->detail, sizeof(out->detail), "devfreq / %s", gov);
    return true;
}

bool gpu_set(GpuState *out, int mode, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!out || !out->available || mode < 0 || mode > 2) {
        if (error && error_size) snprintf(error, error_size, "当前系统不支持 GPU 性能模式");
        return false;
    }
    if (!out->modes[mode]) {
        if (error && error_size)
            snprintf(error, error_size, "当前 GPU 不支持%s模式", gpu_mode_label(mode));
        return false;
    }

    const char *fake = getenv("TAIJI_TEST_GPU");
    if (fake && *fake) {
        out->mode = mode;
        const char *gov[] = {"simple_ondemand", "powersave", "performance"};
        snprintf(out->detail, sizeof(out->detail), "devfreq / %s", gov[mode]);
        return true;
    }

    bool ok = false;
    if (strcmp(out->backend, "devfreq") == 0) {
        const char *gov = mode == GPU_MODE_SAVER ? out->saver_governor :
                          (mode == GPU_MODE_PERFORMANCE ? out->perf_governor :
                           out->auto_governor);
        if (!gov || !*gov) gov = out->original_governor;
        ok = privileged_write(out->governor_path, gov);
        if (ok) ok = verify_devfreq(out, mode);
    } else if (strcmp(out->backend, "kgsl") == 0) {
        int min_level = 0, max_level = 0;
        if (mode == GPU_MODE_SAVER) {
            min_level = out->max_level;
            max_level = out->max_level;
        } else if (mode == GPU_MODE_PERFORMANCE) {
            min_level = 0;
            max_level = 0;
        } else {
            min_level = out->orig_min_level;
            max_level = out->orig_max_level;
        }
        char min_str[16], max_str[16];
        snprintf(min_str, sizeof(min_str), "%d", min_level);
        snprintf(max_str, sizeof(max_str), "%d", max_level);
        bool ok_min = privileged_write(out->min_level_path, min_str);
        bool ok_max = privileged_write(out->max_level_path, max_str);
        int got_min = read_int_file(out->min_level_path);
        int got_max = read_int_file(out->max_level_path);
        ok = ok_min && ok_max && got_min == min_level && got_max == max_level;
        if (ok) {
            out->mode = mode;
            snprintf(out->detail, sizeof(out->detail), "KGSL / pwrlevel %d-%d",
                     min_level, max_level);
        }
    } else if (strcmp(out->backend, "amdgpu") == 0) {
        const char *level = mode == GPU_MODE_SAVER ? "low" :
                            (mode == GPU_MODE_PERFORMANCE ? "high" : "auto");
        ok = privileged_write(out->governor_path, level);
        if (ok) {
            char got[64];
            read_text_file(out->governor_path, got, sizeof(got));
            ok = str_ieq(got, level);
            if (ok) {
                out->mode = mode;
                snprintf(out->detail, sizeof(out->detail), "amdgpu / %s", got);
            }
        }
    }

    if (!ok) {
        if (error && error_size)
            snprintf(error, error_size,
                     "GPU 模式切换失败，需要写入驱动的权限；可配置 sudo/udev 规则或使用 root 运行");
        return false;
    }
    if (strcmp(out->backend, "devfreq") == 0) out->mode = mode;
    return true;
}

#endif
