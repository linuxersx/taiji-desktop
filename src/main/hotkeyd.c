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
#ifndef BTN_TRIGGER_HAPPY1
#define BTN_TRIGGER_HAPPY1 0x2c0
#endif
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP 0x220
#endif
#ifndef BTN_DPAD_DOWN
#define BTN_DPAD_DOWN 0x221
#endif
#ifndef ABS_HAT0Y
#define ABS_HAT0Y 0x11
#endif
#ifndef KEY_RECORD
#define KEY_RECORD 0x1a7
#endif
#define BRIGHTNESS_STEP 10
#define POWER_LONG_MS 1500
#define GUIDE_LONG_MS 600
#define LONG_BITS (sizeof(unsigned long) * 8)

#define MAX_DEVICES 32
#define DEBOUNCE_MS 300

typedef struct {
    int fd;
    char path[64];
    char name[128];
    char uniq[64];
    char phys[64];
    bool is_pad;
    bool is_touchpad;
    bool has_hat;
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
static bool g_share_held = false;
static bool g_share_source_grabbed = false;
static int g_share_source_fd = -1;
static int g_share_grab_fd = -1;

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

static bool device_roles(int fd, const char *name, bool *is_pad,
                         bool *is_touchpad, bool *has_hat)
{
    unsigned long evbit[(EV_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    unsigned long keybit[(KEY_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    unsigned long absbit[(ABS_MAX + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) return false;
    if (!bit_is_set(evbit, EV_KEY)) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) return false;
    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);

    bool pad = bit_is_set(keybit, BTN_MODE) ||
               bit_is_set(keybit, KEY_HOME) ||
               bit_is_set(keybit, KEY_POWER) ||
               bit_is_set(keybit, BTN_TRIGGER_HAPPY1) ||
               bit_is_set(keybit, BTN_DPAD_UP) ||
               bit_is_set(absbit, ABS_HAT0Y);
    bool touchpad = false;
    if (name && (strcasestr(name, "touchpad") || strcasestr(name, "trackpad")))
        touchpad = true;
    else if (bit_is_set(keybit, BTN_LEFT) &&
             bit_is_set(absbit, ABS_MT_POSITION_X))
        touchpad = true;

    *is_pad = pad;
    *is_touchpad = touchpad;
    *has_hat = bit_is_set(absbit, ABS_HAT0Y);
    return pad || touchpad;
}

static void base_controller_name(const char *name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", name ? name : "");
    char *p = strcasestr(out, " touchpad");
    if (p) *p = '\0';
    p = strcasestr(out, " trackpad");
    if (p) *p = '\0';
}

static bool same_controller(const Device *a, const Device *b)
{
    if (a->uniq[0] && b->uniq[0] && strcasecmp(a->uniq, b->uniq) == 0)
        return true;

    char na[128], nb[128];
    base_controller_name(a->name, na, sizeof(na));
    base_controller_name(b->name, nb, sizeof(nb));
    if (na[0] && nb[0] && strcasecmp(na, nb) == 0) return true;

    if (a->phys[0] && b->phys[0]) {
        char pa[64], pb[64];
        snprintf(pa, sizeof(pa), "%s", a->phys);
        snprintf(pb, sizeof(pb), "%s", b->phys);
        char *slash = strrchr(pa, '/');
        if (slash) *slash = '\0';
        slash = strrchr(pb, '/');
        if (slash) *slash = '\0';
        if (pa[0] && strcasecmp(pa, pb) == 0) return true;
    }
    return false;
}

static int find_pad_for(const Device *d)
{
    if (d->is_pad) return d->fd;
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].fd < 0 || !g_devices[i].is_pad) continue;
        if (same_controller(d, &g_devices[i])) return g_devices[i].fd;
    }
    return -1;
}

static bool is_share_code(const Device *d, int code)
{
    if (d->is_touchpad) return code == BTN_LEFT;
    if (code == KEY_RECORD) return true;
    if (code != BTN_TRIGGER_HAPPY1) return false;
    /* Some xpad versions map the d-pad to HAPPY1..HAPPY4.  Only treat HAPPY1
     * as Share when the pad has a separate hat d-pad, or is a Switch pad. */
    if (d->has_hat) return true;
    return d->name[0] &&
           (strcasestr(d->name, "switch") || strcasestr(d->name, "nintendo"));
}

static void release_share(void)
{
    if (!g_share_held) return;
    if (g_share_source_grabbed && g_share_source_fd >= 0 &&
        g_share_source_fd != g_share_grab_fd)
        ioctl(g_share_source_fd, EVIOCGRAB, 0);
    if (g_share_grab_fd >= 0) ioctl(g_share_grab_fd, EVIOCGRAB, 0);
    fprintf(stderr, "[taiji-hotkeyd] share release (grab fd %d)\n",
            g_share_grab_fd);
    g_share_held = false;
    g_share_source_grabbed = false;
    g_share_source_fd = -1;
    g_share_grab_fd = -1;
}

static void close_device(Device *d)
{
    if (d->fd >= 0 && (d->fd == g_share_source_fd || d->fd == g_share_grab_fd))
        release_share();
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
        char name[128] = "unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        name[sizeof(name) - 1] = '\0';
        char uniq[64] = "";
        char phys[64] = "";
        ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq);
        uniq[sizeof(uniq) - 1] = '\0';
        ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
        phys[sizeof(phys) - 1] = '\0';
        bool is_pad = false, is_touchpad = false, has_hat = false;
        if (!device_roles(fd, name, &is_pad, &is_touchpad, &has_hat)) {
            close(fd);
            continue;
        }
        fprintf(stderr, "[taiji-hotkeyd] watching %s (%s, %s%s)\n",
                path, name,
                is_pad ? "pad" : "",
                is_touchpad ? (is_pad ? "+touchpad" : "touchpad") : "");
        Device *dev = &g_devices[g_count];
        memset(dev, 0, sizeof(*dev));
        dev->fd = fd;
        snprintf(dev->path, sizeof(dev->path), "%s", path);
        snprintf(dev->name, sizeof(dev->name), "%s", name);
        snprintf(dev->uniq, sizeof(dev->uniq), "%s", uniq);
        snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
        dev->is_pad = is_pad;
        dev->is_touchpad = is_touchpad;
        dev->has_hat = has_hat;
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

static void trigger_brightness_request(int delta)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char dir[512];
    char path[600];
    if (runtime && *runtime) snprintf(dir, sizeof(dir), "%s/taiji", runtime);
    else snprintf(dir, sizeof(dir), "/tmp/taiji");
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/brightness_request", dir);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", delta);
        if (n > 0) (void)write(fd, buf, (size_t)n);
        close(fd);
    }
    fprintf(stderr, "[taiji-hotkeyd] capture brightness %+d%% -> %s\n", delta, path);
}

static void handle_event(const struct input_event *ev, const Device *d)
{
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_HAT0Y && g_share_held &&
            d->fd == g_share_grab_fd) {
            if (ev->value < 0) trigger_brightness_request(BRIGHTNESS_STEP);
            else if (ev->value > 0) trigger_brightness_request(-BRIGHTNESS_STEP);
        }
        return;
    }
    if (ev->type != EV_KEY) return;

    /* Switch Pro Capture, Xbox Share, or PS touchpad click.  The latter may be
     * on a separate evdev node, so grab the paired gamepad device instead. */
    if (is_share_code(d, ev->code)) {
        if (ev->value == 1) {
            if (!g_share_held) {
                int pad_fd = find_pad_for(d);
                bool source_grabbed = false;
                if (pad_fd >= 0 && pad_fd != d->fd)
                    source_grabbed = ioctl(d->fd, EVIOCGRAB, 1) == 0;
                if (pad_fd >= 0 && ioctl(pad_fd, EVIOCGRAB, 1) == 0) {
                    g_share_held = true;
                    g_share_source_fd = d->fd;
                    g_share_source_grabbed = source_grabbed;
                    g_share_grab_fd = pad_fd;
                    fprintf(stderr,
                            "[taiji-hotkeyd] share grab %s via %s\n",
                            g_share_grab_fd == d->fd ? d->path : "paired pad",
                            d->path);
                } else {
                    if (source_grabbed)
                        ioctl(d->fd, EVIOCGRAB, 0);
                    fprintf(stderr,
                            "[taiji-hotkeyd] share grab failed %s: %s\n",
                            d->path, pad_fd < 0 ? "no paired pad" : strerror(errno));
                }
            }
        } else if (ev->value == 0 && g_share_held &&
                   d->fd == g_share_source_fd) {
            release_share();
        }
        return;
    }

    if (d->is_touchpad) return;

    /* While the share key is held the paired pad is grabbed so the foreground
     * game does not also receive the d-pad presses. */
    if (g_share_held && d->fd == g_share_grab_fd) {
        if (ev->code == BTN_DPAD_UP && ev->value == 1)
            trigger_brightness_request(BRIGHTNESS_STEP);
        else if (ev->code == BTN_DPAD_DOWN && ev->value == 1)
            trigger_brightness_request(-BRIGHTNESS_STEP);
        return;
    }

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
                short re = pfds[i].revents;
                if (!re) continue;

                /* The event node can be destroyed and recreated (uinput,
                 * virtual pads, controller daemons).  The old fd then points
                 * to a deleted inode and poll() returns POLLHUP immediately.
                 * Without this branch the loop spins at 100% CPU and the new
                 * node is never opened because the path still looks present. */
                if (re & (POLLHUP | POLLERR | POLLNVAL)) {
                    close_device(&active[i]);
                    for (int j = 0; j < g_count; j++) {
                        if (strcmp(g_devices[j].path, active[i].path) == 0) {
                            g_devices[j].fd = -1;
                        }
                    }
                    last_scan = 0; /* rescan immediately */
                    continue;
                }

                if (!(re & POLLIN)) continue;
                struct input_event ev[32];
                ssize_t got;
                while ((got = read(active[i].fd, ev, sizeof(ev))) > 0) {
                    int count = (int)(got / (ssize_t)sizeof(ev[0]));
                    for (int k = 0; k < count; k++) handle_event(&ev[k], &active[i]);
                }
                if (got == 0 ||
                    (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close_device(&active[i]);
                    for (int j = 0; j < g_count; j++) {
                        if (strcmp(g_devices[j].path, active[i].path) == 0) {
                            g_devices[j].fd = -1;
                        }
                    }
                    last_scan = 0;
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
