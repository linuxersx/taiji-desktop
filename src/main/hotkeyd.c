// SPDX-License-Identifier: GPL-3.0-or-later
/* Taiji controller hotkey daemon.
 *
 * Reads evdev without grabbing the device, so the foreground application keeps
 * receiving input. Xbox Guide is usually BTN_MODE; several Switch drivers expose
 * Home as BTN_MODE or KEY_HOME. Both are handled.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BTN_MODE
#define BTN_MODE 0x13c
#endif
#ifndef KEY_HOME
#define KEY_HOME 102
#endif
#ifndef KEY_POWER
#define KEY_POWER 116
#endif
#define POWER_LONG_MS 1500
#define GUIDE_LONG_MS 600
#define LONG_BITS (sizeof(unsigned long) * 8)

#define MAX_DEVICES 32
#define DEBOUNCE_MS 300

typedef struct {
    int fd;
    char path[64];
} Device;

static volatile sig_atomic_t g_stop = 0;
static Device g_devices[MAX_DEVICES];
static int g_count = 0;
static int64_t g_last_trigger = 0;
static int64_t g_power_down_at = 0;
static bool g_power_long_fired = false;
static int64_t g_guide_down_at = 0;
static bool g_guide_long_fired = false;
static bool g_select_down = false;

static bool osk_visible(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    const char *wd = getenv("WAYLAND_DISPLAY");
    char safe[96] = "default";
    if (wd && *wd) {
        size_t j = 0;
        if (sid && *sid) {
            for (const char *p = sid; *p && j + 1 < sizeof(safe); p++)
                safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
            if (j + 1 < sizeof(safe)) safe[j++] = '-';
        }
        for (const char *p = wd; *p && j + 1 < sizeof(safe); p++)
            safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
        safe[j] = 0;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/taiji/osk-%s.visible",
             runtime && *runtime ? runtime : "/tmp", safe);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char c = 0;
    if (fread(&c, 1, 1, f) != 1) c = 0;
    fclose(f);
    return c == '1';
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static bool bit_is_set(const unsigned long *bits, int code)
{
    return ((bits[code / LONG_BITS] >> (code % LONG_BITS)) & 1UL) != 0;
}

static bool device_has_hotkey(int fd)
{
    unsigned long evbit[(EV_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    unsigned long keybit[(KEY_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) return false;
    if (!bit_is_set(evbit, EV_KEY)) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) return false;
    return bit_is_set(keybit, BTN_MODE) ||
           bit_is_set(keybit, KEY_HOME) ||
           bit_is_set(keybit, KEY_POWER);
}

static void close_device(Device *d)
{
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
}

static void scan_devices(void)
{
    for (int i = 0; i < 32 && g_count < MAX_DEVICES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        bool already = false;
        for (int j = 0; j < g_count; j++) {
            if (g_devices[j].fd >= 0 && strcmp(g_devices[j].path, path) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!device_has_hotkey(fd)) {
            close(fd);
            continue;
        }
        char name[128] = "unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        name[sizeof(name) - 1] = '\0';
        fprintf(stderr, "[taiji-hotkeyd] watching %s (%s)\n", path, name);
        g_devices[g_count].fd = fd;
        snprintf(g_devices[g_count].path, sizeof(g_devices[g_count].path), "%s", path);
        g_count++;
    }
}

static void trigger_return(void)
{
    int64_t t = now_ms();
    if (t - g_last_trigger < DEBOUNCE_MS) return;
    g_last_trigger = t;

    const char *cmd = getenv("TAIJI_RETURN_CMD");
    if (!cmd || !*cmd) cmd = "/usr/libexec/taiji/taiji-return";

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void trigger_overview(void)
{
    const char *cmd = getenv("TAIJI_OVERVIEW_CMD");
    if (!cmd || !*cmd) cmd = "/usr/libexec/taiji/taiji-overview";

    fprintf(stderr, "[taiji-hotkeyd] guide long press -> overview\n");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void trigger_power_request(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char dir[512];
    char path[600];
    if (runtime && *runtime) snprintf(dir, sizeof(dir), "%s/taiji", runtime);
    else snprintf(dir, sizeof(dir), "/tmp/taiji");
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/power_request", dir);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)write(fd, "1\n", 2);
        close(fd);
    }
    fprintf(stderr, "[taiji-hotkeyd] power long press -> %s\n", path);
}

static void handle_event(const struct input_event *ev, const Device *d)
{
    if (ev->type != EV_KEY) return;
    if (ev->code == KEY_POWER) {
        if (ev->value == 1) {
            g_power_down_at = now_ms();
            g_power_long_fired = false;
        } else if (ev->value == 0) {
            g_power_down_at = 0;
            g_power_long_fired = false;
        }
        return;
    }
    if (ev->code == BTN_SELECT) {
        g_select_down = ev->value != 0;
        return;
    }
    if (ev->code != BTN_MODE && ev->code != KEY_HOME) return;
    if (ev->value == 0 && (g_select_down || osk_visible())) {
        g_guide_down_at = 0;
        g_guide_long_fired = false;
        return;
    }
    if (ev->value == 1) {
        g_guide_down_at = now_ms();
        g_guide_long_fired = false;
        return;
    }
    if (ev->value == 0 && g_guide_down_at) {
        int64_t held = now_ms() - g_guide_down_at;
        bool long_fired = g_guide_long_fired;
        g_guide_down_at = 0;
        g_guide_long_fired = false;
        if (long_fired) return;
        if (held >= GUIDE_LONG_MS) return;
        fprintf(stderr, "[taiji-hotkeyd] guide short press (%lldms) from %s\n",
                (long long)held, d->path);
        trigger_return();
    }
}

static int acquire_lock(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[512];
    if (runtime && *runtime)
        snprintf(path, sizeof(path), "%s/taiji", runtime);
    else
        snprintf(path, sizeof(path), "/tmp/taiji");
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/taiji-hotkeyd.lock",
             runtime && *runtime ? runtime : "/tmp");
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void)
{
    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "[taiji-hotkeyd] another instance is already running\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, SIG_IGN);

    fprintf(stderr, "[taiji-hotkeyd] started\n");
    scan_devices();
    if (g_count == 0) {
        fprintf(stderr, "[taiji-hotkeyd] no controller with BTN_MODE/KEY_HOME found "
                        "(check input permissions / udev uaccess)\n");
    }

    int64_t last_scan = now_ms();
    struct pollfd pfds[MAX_DEVICES];
    Device active[MAX_DEVICES];

    while (!g_stop) {
        int n = 0;
        for (int i = 0; i < g_count; i++) {
            if (g_devices[i].fd < 0) continue;
            pfds[n].fd = g_devices[i].fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            active[n] = g_devices[i];
            n++;
        }

        int rc = poll(pfds, (nfds_t)n, 100);
        if (rc > 0) {
            for (int i = 0; i < n; i++) {
                if (!(pfds[i].revents & POLLIN)) continue;
                struct input_event ev[32];
                ssize_t got;
                while ((got = read(active[i].fd, ev, sizeof(ev))) > 0) {
                    int count = (int)(got / (ssize_t)sizeof(ev[0]));
                    for (int k = 0; k < count; k++) handle_event(&ev[k], &active[i]);
                }
                if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close_device(&active[i]);
                    for (int j = 0; j < g_count; j++) {
                        if (strcmp(g_devices[j].path, active[i].path) == 0) {
                            g_devices[j].fd = -1;
                        }
                    }
                }
            }
        }

        if (g_power_down_at && !g_power_long_fired &&
            now_ms() - g_power_down_at >= POWER_LONG_MS) {
            g_power_long_fired = true;
            trigger_power_request();
        }

        if (g_guide_down_at && !g_guide_long_fired &&
            now_ms() - g_guide_down_at >= GUIDE_LONG_MS) {
            g_guide_long_fired = true;
            trigger_overview();
        }

        if (now_ms() - last_scan > 3000) {
            last_scan = now_ms();
            for (int i = 0; i < g_count; ) {
                if (g_devices[i].fd < 0) {
                    g_devices[i] = g_devices[g_count - 1];
                    g_count--;
                } else {
                    i++;
                }
            }
            scan_devices();
        }
    }

    for (int i = 0; i < g_count; i++) close_device(&g_devices[i]);
    close(lock_fd);
    fprintf(stderr, "[taiji-hotkeyd] stopped\n");
    return 0;
}
