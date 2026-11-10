// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "system_brightness.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[512];
static int g_max = 0;
static bool g_probed = false;

static int read_int_file(const char *path)
{
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return -1;
    char *end = NULL;
    long v = strtol(data, &end, 10);
    free(data);
    if (end == data || v < 0) return -1;
    return (int)v;
}

static bool probe(void)
{
    if (g_probed) return g_dir[0] != '\0';
    g_probed = true;
    const char *base = getenv("TAIJI_BACKLIGHT_DIR");
    if (!base || !*base) base = "/sys/class/backlight";
    int count = 0;
    char **items = list_dir(base, &count);
    if (!items) return false;
    for (int i = 0; i < count; i++) {
        char *dir = path_join(base, items[i]);
        char *bpath = path_join(dir, "brightness");
        char *mpath = path_join(dir, "max_brightness");
        int max = read_int_file(mpath);
        if (max > 0 && access(bpath, F_OK) == 0) {
            snprintf(g_dir, sizeof(g_dir), "%s", dir);
            g_max = max;
            free(bpath);
            free(mpath);
            free(dir);
            break;
        }
        free(bpath);
        free(mpath);
        free(dir);
    }
    free_strv(items, count);
    return g_dir[0] != '\0';
}

bool system_brightness_available(void)
{
    return probe();
}

int system_brightness_get(void)
{
    if (!probe()) return -1;
    char path[600];
    snprintf(path, sizeof(path), "%s/brightness", g_dir);
    int value = read_int_file(path);
    if (value < 0) return -1;
    int pct = (value * 100 + g_max / 2) / g_max;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

bool system_brightness_set(int percent)
{
    if (!probe()) return false;
    if (percent < 1) percent = 1;      /* never turn the panel fully off */
    if (percent > 100) percent = 100;
    int value = (percent * g_max + 50) / 100;
    if (value < 1) value = 1;
    if (value > g_max) value = g_max;

    char path[600];
    snprintf(path, sizeof(path), "%s/brightness", g_dir);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == (ssize_t)n;
}
