// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "system_volume.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define VOLUME_BACKEND "CoreAudio"
#else
#define VOLUME_BACKEND "PipeWire"
#endif

enum {
    VOL_BACKEND_NONE = 0,
    VOL_BACKEND_WPCTL,
    VOL_BACKEND_PACTL,
    VOL_BACKEND_OSASCRIPT
};

static int g_backend = -1;

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

static int detect_backend(void)
{
    if (g_backend >= 0) return g_backend;
#ifdef __APPLE__
    g_backend = command_exists("osascript") ? VOL_BACKEND_OSASCRIPT
                                            : VOL_BACKEND_NONE;
#else
    if (command_exists("wpctl")) g_backend = VOL_BACKEND_WPCTL;
    else if (command_exists("pactl")) g_backend = VOL_BACKEND_PACTL;
    else g_backend = VOL_BACKEND_NONE;
#endif
    return g_backend;
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

static bool read_cmd(const char *cmd, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool got = fgets(out, (int)out_size, f) != NULL;
    pclose(f);
    if (!got) return false;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

/* ── Asynchronous command worker ─────────────────────────────────────────────
 * Volume keys and slider dragging can fire much faster than wpctl/pactl can
 * process changes.  Commands run on a worker thread and only the latest
 * requested value is kept, so the UI stays responsive and the final system
 * volume always matches the slider. */

typedef struct {
    int percent; /* -1 means "leave unchanged" */
    int mute;    /* -1 means "leave unchanged", 0 = unmuted, 1 = muted */
} VolumeCommand;

static pthread_mutex_t g_vm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_vc = PTHREAD_COND_INITIALIZER;
static VolumeCommand g_vcmd = {-1, -1};
static bool g_vpending = false;
static bool g_vworker_started = false;
static uint64_t g_vlast_apply_ms = 0;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void execute_volume(int percent)
{
    char cmd[256];
    if (g_backend == VOL_BACKEND_OSASCRIPT) {
        snprintf(cmd, sizeof(cmd),
                 "osascript -e 'set volume output volume %d' 2>/dev/null",
                 percent);
        run_cmd(cmd);
    } else if (g_backend == VOL_BACKEND_WPCTL) {
        snprintf(cmd, sizeof(cmd),
                 "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%% 2>/dev/null",
                 percent);
        run_cmd(cmd);
    } else if (g_backend == VOL_BACKEND_PACTL) {
        snprintf(cmd, sizeof(cmd),
                 "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null",
                 percent);
        run_cmd(cmd);
    }
}

static void execute_mute(bool muted)
{
    char cmd[256];
    if (g_backend == VOL_BACKEND_OSASCRIPT) {
        snprintf(cmd, sizeof(cmd),
                 "osascript -e 'set volume output muted %s' 2>/dev/null",
                 muted ? "true" : "false");
        run_cmd(cmd);
    } else if (g_backend == VOL_BACKEND_WPCTL) {
        snprintf(cmd, sizeof(cmd),
                 "wpctl set-mute @DEFAULT_AUDIO_SINK@ %d 2>/dev/null",
                 muted ? 1 : 0);
        run_cmd(cmd);
    } else if (g_backend == VOL_BACKEND_PACTL) {
        snprintf(cmd, sizeof(cmd),
                 "pactl set-sink-mute @DEFAULT_SINK@ %d 2>/dev/null",
                 muted ? 1 : 0);
        run_cmd(cmd);
    }
}

static void *volume_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_vm);
        while (!g_vpending) pthread_cond_wait(&g_vc, &g_vm);
        VolumeCommand cmd = g_vcmd;
        g_vpending = false;
        g_vcmd.percent = -1;
        g_vcmd.mute = -1;
        pthread_mutex_unlock(&g_vm);

        if (cmd.mute >= 0) execute_mute(cmd.mute != 0);
        if (cmd.percent >= 0) execute_volume(cmd.percent);
    }
    return NULL;
}

static void queue_volume_command(int percent, int mute)
{
    pthread_mutex_lock(&g_vm);
    g_vcmd.percent = percent;
    g_vcmd.mute = mute;
    g_vpending = true;
    g_vlast_apply_ms = monotonic_ms();
    if (!g_vworker_started) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, volume_worker, NULL) == 0) {
            pthread_detach(worker);
            g_vworker_started = true;
        }
    }
    pthread_cond_signal(&g_vc);
    pthread_mutex_unlock(&g_vm);
}

static bool volume_apply_recent(void)
{
    pthread_mutex_lock(&g_vm);
    bool recent = g_vworker_started &&
                  monotonic_ms() - g_vlast_apply_ms < 700u;
    pthread_mutex_unlock(&g_vm);
    return recent;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void system_volume_init(SystemVolume *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->percent = 0;
    detect_backend();
    if (g_backend == VOL_BACKEND_PACTL)
        snprintf(out->backend, sizeof(out->backend), "PulseAudio");
    else if (g_backend == VOL_BACKEND_NONE)
        snprintf(out->backend, sizeof(out->backend), "%s", VOLUME_BACKEND);
    else
        snprintf(out->backend, sizeof(out->backend), "%s", VOLUME_BACKEND);
}

#ifdef __APPLE__

void system_volume_poll(SystemVolume *out)
{
    if (!out) return;
    if (detect_backend() == VOL_BACKEND_NONE) { out->available = false; return; }
    /* Do not overwrite the freshly requested value while the worker is still
     * applying it; the next poll (or key press) will resynchronise. */
    if (volume_apply_recent()) return;
    out->available = false;
    char buf[128];
    if (read_cmd("osascript -e 'output volume of (get volume settings)' 2>/dev/null",
                 buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0 && v <= 100) {
            out->percent = v;
            out->available = true;
        }
    }
    if (read_cmd("osascript -e 'output muted of (get volume settings)' 2>/dev/null",
                 buf, sizeof(buf))) {
        out->muted = strstr(buf, "true") != NULL;
    }
}

#else

static bool poll_wpctl(SystemVolume *out)
{
    char buf[256];
    if (!read_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null",
                  buf, sizeof(buf)))
        return false;
    const char *p = strstr(buf, "Volume:");
    if (!p) return false;
    float f = 0.0f;
    if (sscanf(p + 7, "%f", &f) != 1) return false;
    int v = (int)lroundf(f * 100.0f);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    out->percent = v;
    out->muted = strstr(buf, "[MUTED]") != NULL;
    out->available = true;
    snprintf(out->backend, sizeof(out->backend), "PipeWire");
    return true;
}

static bool poll_pactl(SystemVolume *out)
{
    char buf[256];
    if (!read_cmd("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null",
                  buf, sizeof(buf)))
        return false;
    char *pct = strchr(buf, '%');
    if (!pct) return false;
    int end = (int)(pct - buf);
    int start = end;
    while (start > 0 && isdigit((unsigned char)buf[start - 1])) start--;
    if (start >= end) return false;
    int v = atoi(buf + start);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    out->percent = v;
    out->muted = false;
    if (read_cmd("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null",
                 buf, sizeof(buf)))
        out->muted = strstr(buf, "yes") != NULL;
    out->available = true;
    snprintf(out->backend, sizeof(out->backend), "PulseAudio");
    return true;
}

void system_volume_poll(SystemVolume *out)
{
    if (!out) return;
    if (detect_backend() == VOL_BACKEND_NONE) { out->available = false; return; }
    if (volume_apply_recent()) return;
    out->available = false;
    if (g_backend == VOL_BACKEND_WPCTL) {
        if (poll_wpctl(out)) return;
        if (command_exists("pactl")) poll_pactl(out);
        return;
    }
    if (g_backend == VOL_BACKEND_PACTL) poll_pactl(out);
}

#endif

bool system_volume_apply(SystemVolume *out, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (detect_backend() == VOL_BACKEND_NONE) return false;
    queue_volume_command(percent, percent > 0 ? 0 : -1);
    if (out) {
        out->available = true;
        out->percent = percent;
        out->muted = percent <= 0;
    }
    return true;
}

bool system_volume_set(SystemVolume *out, int percent)
{
    return system_volume_apply(out, percent);
}

bool system_volume_toggle_mute(SystemVolume *out)
{
    if (detect_backend() == VOL_BACKEND_NONE) return false;
    bool mute = !(out && out->muted);
    queue_volume_command(-1, mute ? 1 : 0);
    if (out) out->muted = mute;
    return true;
}

bool system_volume_delta(SystemVolume *out, int delta)
{
    if (!out || !out->available) return false;
    int target = out->percent + delta;
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    return system_volume_apply(out, target);
}
