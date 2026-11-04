// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "desktop_watch.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#include <poll.h>

#define WATCH_MAX 16
#define DEBOUNCE_MS 350
#define FALLBACK_MS 5000

static int g_fd = -1;
static int g_wd[WATCH_MAX];
static char g_paths[WATCH_MAX][4096];
static int g_count;
static int64_t g_last_event_ms;
static int64_t g_last_fallback_ms;
static bool g_dirty;
static uint64_t g_signature;
static bool g_reinit;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void add_path(const char *path, uint32_t mask)
{
    if (!path || !*path || g_count >= WATCH_MAX) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    int wd = inotify_add_watch(g_fd, path, mask);
    if (wd < 0) return;
    snprintf(g_paths[g_count], sizeof(g_paths[g_count]), "%s", path);
    g_wd[g_count] = wd;
    g_count++;
}

static void add_watches(void)
{
    g_count = 0;
    uint32_t dir_mask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
                        IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;
    uint32_t file_mask = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |
                         IN_DELETE_SELF | IN_MOVE_SELF;

    add_path("/usr/share/applications", dir_mask);
    add_path("/usr/local/share/applications", dir_mask);
    const char *home = getenv("HOME");
    char path[4096];
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.local/share/applications", home);
        add_path(path, dir_mask);
        snprintf(path, sizeof(path), "%s/.local/share/flatpak/exports/share/applications", home);
        add_path(path, dir_mask);
    }
    add_path("/var/lib/flatpak/exports/share/applications", dir_mask);
    add_path("/var/lib/snapd/desktop/applications", dir_mask);
    add_path("/var/log/pacman.log", file_mask);
    add_path("/var/lib/pacman/sync", dir_mask);
    add_path("/var/lib/pacman/local", dir_mask);
}

static uint64_t signature(void)
{
    uint64_t h = 1469598103934665603ULL;
    struct stat st;
    const char *home = getenv("HOME");
    char path[4096];
    const char *paths[] = {
        "/usr/share/applications", "/usr/local/share/applications",
        "/var/lib/flatpak/exports/share/applications",
        "/var/lib/snapd/desktop/applications",
        "/var/log/pacman.log", "/var/lib/pacman/sync",
        "/var/lib/pacman/local",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (stat(paths[i], &st) == 0) {
            h ^= (uint64_t)st.st_mtime;
            h *= 1099511628211ULL;
            h ^= (uint64_t)st.st_size;
            h *= 1099511628211ULL;
        }
    }
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.local/share/applications", home);
        if (stat(path, &st) == 0) {
            h ^= (uint64_t)st.st_mtime; h *= 1099511628211ULL;
            h ^= (uint64_t)st.st_size; h *= 1099511628211ULL;
        }
        snprintf(path, sizeof(path), "%s/.local/share/flatpak/exports/share/applications", home);
        if (stat(path, &st) == 0) {
            h ^= (uint64_t)st.st_mtime; h *= 1099511628211ULL;
            h ^= (uint64_t)st.st_size; h *= 1099511628211ULL;
        }
    }
    return h;
}

void desktop_watch_init(void)
{
    desktop_watch_close();
    g_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    g_reinit = false;
    g_last_event_ms = 0;
    g_last_fallback_ms = 0;
    g_dirty = false;
    g_signature = signature();
    if (g_fd < 0) return;
    add_watches();
}

void desktop_watch_close(void)
{
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_count = 0;
}

bool desktop_watch_poll(void)
{
    int64_t now = now_ms();
    if (g_fd < 0) {
        if (now - g_last_fallback_ms < FALLBACK_MS) return false;
        g_last_fallback_ms = now;
        uint64_t sig = signature();
        bool changed = sig != g_signature;
        g_signature = sig;
        return changed;
    }

    char buf[4096];
    bool got = false;
    for (;;) {
        ssize_t n = read(g_fd, buf, sizeof(buf));
        if (n <= 0) break;
        got = true;
        for (ssize_t off = 0; off < n;) {
            const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
            if (ev->mask & IN_IGNORED) g_reinit = true;
            if (ev->mask & IN_Q_OVERFLOW) g_reinit = true;
            off += (ssize_t)sizeof(*ev) + ev->len;
        }
    }
    if (got) {
        g_last_event_ms = now;
        g_dirty = true;
    }
    if (g_reinit && !got) {
        g_reinit = false;
        add_watches();
    }
    if (g_dirty && now - g_last_event_ms >= DEBOUNCE_MS) {
        g_dirty = false;
        g_last_event_ms = 0;
        g_signature = signature();
        return true;
    }
    /* A watched directory may have been created after startup, so inotify
     * cannot have a watch on it yet. A lightweight signature check catches
     * that case without rescanning application directories. */
    if (!g_dirty && now - g_last_fallback_ms >= FALLBACK_MS) {
        g_last_fallback_ms = now;
        uint64_t sig = signature();
        if (sig != g_signature) {
            g_signature = sig;
            g_reinit = true;
            return true;
        }
    }
    return false;
}

#else

void desktop_watch_init(void) {}
void desktop_watch_close(void) {}
bool desktop_watch_poll(void) { return false; }

#endif
