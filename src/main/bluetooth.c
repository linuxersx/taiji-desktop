// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "bluetooth.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
static bool command_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char *p = path_join(dir, name);
        bool ok = access(p, X_OK) == 0;
        free(p);
        if (ok) { free(copy); return true; }
    }
    free(copy);
    return false;
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

static void run_capture(const char *cmd, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return;
    size_t len = 0;
    size_t n;
    while (len + 1 < out_size &&
           (n = fread(out + len, 1, out_size - 1 - len, f)) > 0) {
        len += n;
    }
    out[len] = '\0';
    pclose(f);
}

static void trim_line(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void parse_devices(const char *buf, BtState *st, bool connected, bool paired)
{
    const char *p = buf;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        trim_line(line);
        if (strncmp(line, "Device ", 7) == 0) {
            char *addr = line + 7;
            char *name = strchr(addr, ' ');
            if (name) {
                *name++ = '\0';
                trim_line(name);
            }
            trim_line(addr);
            if (*addr) {
                int found = -1;
                for (int i = 0; i < st->count; i++) {
                    if (strcmp(st->items[i].address, addr) == 0) { found = i; break; }
                }
                if (found < 0 && st->count < (int)(sizeof(st->items) / sizeof(st->items[0]))) {
                    found = st->count++;
                    memset(&st->items[found], 0, sizeof(st->items[found]));
                    snprintf(st->items[found].address,
                             sizeof(st->items[found].address), "%s", addr);
                }
                if (found >= 0) {
                    BtDevice *d = &st->items[found];
                    if (name && *name) snprintf(d->name, sizeof(d->name), "%s", name);
                    if (connected) d->connected = true;
                    if (paired) d->paired = true;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static void parse_show(const char *buf, BtState *st)
{
    const char *pow = strstr(buf, "Powered: yes");
    st->enabled = pow != NULL;
    const char *ctrl = strstr(buf, "Controller ");
    if (ctrl) {
        ctrl += 11;
        const char *eol = strchr(ctrl, '\n');
        size_t len = eol ? (size_t)(eol - ctrl) : strlen(ctrl);
        char line[192];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, ctrl, len);
        line[len] = '\0';
        trim_line(line);
        char *paren = strchr(line, '(');
        if (paren) {
            char *end = strrchr(paren, ')');
            if (end) *end = '\0';
            char *n = paren + 1;
            trim_line(n);
            snprintf(st->controller, sizeof(st->controller), "%s", n);
        }
        if (!st->controller[0]) {
            char *sp = strchr(line, ' ');
            if (sp) *sp = '\0';
            snprintf(st->controller, sizeof(st->controller), "%s", line);
        }
    }
}

static int cmp_device(const void *a, const void *b)
{
    const BtDevice *x = a, *y = b;
    if (x->connected != y->connected) return x->connected ? -1 : 1;
    if (x->paired != y->paired) return x->paired ? -1 : 1;
    return strcasecmp(x->name[0] ? x->name : x->address,
                      y->name[0] ? y->name : y->address);
}

static void scan_linux(BtState *st, bool rescan)
{
    st->available = command_exists("bluetoothctl");
    st->enabled = false;
    st->count = 0;
    memset(st->items, 0, sizeof(st->items));
    st->controller[0] = '\0';
    st->message[0] = '\0';

    if (!st->available) {
        snprintf(st->message, sizeof(st->message), "未找到 bluetoothctl / BlueZ");
        return;
    }

    char show[8192];
    run_capture("LC_ALL=C timeout 4 bluetoothctl show 2>/dev/null", show, sizeof(show));
    if (!show[0]) {
        snprintf(st->message, sizeof(st->message), "无法读取 BlueZ 状态");
    } else {
        parse_show(show, st);
    }

    if (!st->enabled) {
        snprintf(st->message, sizeof(st->message), "蓝牙已关闭");
        return;
    }

    if (rescan) {
        run_status("LC_ALL=C timeout 10 bluetoothctl --timeout 7 scan on >/dev/null 2>&1");
    }

    char list[32768];
    run_capture("LC_ALL=C timeout 6 bluetoothctl devices 2>/dev/null", list, sizeof(list));
    parse_devices(list, st, false, false);

    char paired[16384];
    run_capture("LC_ALL=C timeout 6 bluetoothctl devices Paired 2>/dev/null",
                paired, sizeof(paired));
    parse_devices(paired, st, false, true);

    char connected[16384];
    run_capture("LC_ALL=C timeout 6 bluetoothctl devices Connected 2>/dev/null",
                connected, sizeof(connected));
    parse_devices(connected, st, true, true);

    qsort(st->items, (size_t)st->count, sizeof(st->items[0]), cmp_device);

    if (st->count == 0) {
        snprintf(st->message, sizeof(st->message), "没有扫描到蓝牙设备");
    } else {
        snprintf(st->message, sizeof(st->message), "BlueZ");
    }
}
#endif

void bluetooth_scan(BtState *st, bool rescan)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
#ifdef __linux__
    scan_linux(st, rescan);
#else
    (void)rescan;
    snprintf(st->message, sizeof(st->message), "当前系统不支持蓝牙管理");
#endif
}

bool bluetooth_radio(BtState *st)
{
    bool available = false;
    bool enabled = false;
#ifdef __linux__
    available = command_exists("bluetoothctl");
    if (available) {
        char show[8192];
        run_capture("LC_ALL=C timeout 4 bluetoothctl show 2>/dev/null",
                    show, sizeof(show));
        if (show[0]) {
            BtState tmp;
            memset(&tmp, 0, sizeof(tmp));
            parse_show(show, &tmp);
            enabled = tmp.enabled;
            if (st) snprintf(st->controller, sizeof(st->controller), "%s", tmp.controller);
        }
    }
#endif
    if (st) {
        st->available = available;
        st->enabled = enabled;
    }
    return available;
}

static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static BtState g_scan_state;
static bool g_scan_running;
static bool g_scan_ready;
static bool g_scan_rescan;

static void *bt_scan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_scan_mutex);
        bool rescan = g_scan_rescan;
        g_scan_rescan = false;
        pthread_mutex_unlock(&g_scan_mutex);

        BtState result;
        bluetooth_scan(&result, rescan);

        pthread_mutex_lock(&g_scan_mutex);
        g_scan_state = result;
        g_scan_ready = true;
        if (g_scan_rescan) {
            pthread_mutex_unlock(&g_scan_mutex);
            continue; /* a refresh was requested while this scan ran */
        }
        g_scan_running = false;
        pthread_mutex_unlock(&g_scan_mutex);
        break;
    }
    return NULL;
}

void bluetooth_scan_async(bool rescan)
{
    pthread_mutex_lock(&g_scan_mutex);
    if (g_scan_running) {
        if (rescan) g_scan_rescan = true;
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }
    g_scan_running = true;
    g_scan_ready = false;
    g_scan_rescan = rescan;
    pthread_t thread;
    if (pthread_create(&thread, NULL, bt_scan_worker, NULL) != 0) {
        g_scan_running = false;
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }
    pthread_detach(thread);
    pthread_mutex_unlock(&g_scan_mutex);
}

bool bluetooth_scan_take(BtState *state)
{
    if (!state) return false;
    pthread_mutex_lock(&g_scan_mutex);
    if (!g_scan_ready) {
        pthread_mutex_unlock(&g_scan_mutex);
        return false;
    }
    *state = g_scan_state;
    g_scan_ready = false;
    pthread_mutex_unlock(&g_scan_mutex);
    return true;
}

bool bluetooth_toggle(bool enabled, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
#ifdef __linux__
    if (!command_exists("bluetoothctl")) {
        if (error) snprintf(error, error_size, "未找到 bluetoothctl");
        return false;
    }
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "LC_ALL=C timeout 10 bluetoothctl power %s >/dev/null 2>&1",
             enabled ? "on" : "off");
    int rc = run_status(cmd);
    if (rc != 0 && error) snprintf(error, error_size, "BlueZ 切换蓝牙失败");
    return rc == 0;
#else
    (void)enabled;
    if (error) snprintf(error, error_size, "当前系统不支持蓝牙管理");
    return false;
#endif
}

static pthread_mutex_t g_action_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_action_msg[256];
static bool g_action_ready;
static bool g_action_running;
static char g_action_addr[24];

static void *bt_connect_worker(void *arg)
{
    (void)arg;
    char addr[24];
    pthread_mutex_lock(&g_action_mutex);
    snprintf(addr, sizeof(addr), "%s", g_action_addr);
    pthread_mutex_unlock(&g_action_mutex);

    char msg[256];
    snprintf(msg, sizeof(msg), "蓝牙连接失败");
#ifdef __linux__
    char *q = shell_quote(addr);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "LC_ALL=C timeout 40 sh -c '"
             "bluetoothctl --timeout 20 pair %s >/dev/null 2>&1; "
             "bluetoothctl trust %s >/dev/null 2>&1; "
             "bluetoothctl --timeout 20 connect %s 2>&1'",
             q, q, q);
    char out[4096];
    run_capture(cmd, out, sizeof(out));
    if (strstr(out, "Connection successful") || strstr(out, "already connected")) {
        snprintf(msg, sizeof(msg), "蓝牙设备已连接");
    } else {
        const char *line = strstr(out, "Failed to connect");
        if (line) {
            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            if (len >= sizeof(msg)) len = sizeof(msg) - 1;
            memcpy(msg, line, len);
            msg[len] = '\0';
        }
    }
    free(q);
#else
    (void)addr;
    (void)msg;
#endif

    pthread_mutex_lock(&g_action_mutex);
    snprintf(g_action_msg, sizeof(g_action_msg), "%s", msg);
    g_action_ready = true;
    g_action_running = false;
    pthread_mutex_unlock(&g_action_mutex);
    return NULL;
}

void bluetooth_connect_async(const char *address)
{
    if (!address || !*address) return;
    pthread_mutex_lock(&g_action_mutex);
    if (g_action_running) {
        pthread_mutex_unlock(&g_action_mutex);
        return;
    }
    g_action_running = true;
    g_action_ready = false;
    g_action_msg[0] = '\0';
    snprintf(g_action_addr, sizeof(g_action_addr), "%s", address);
    pthread_t thread;
    if (pthread_create(&thread, NULL, bt_connect_worker, NULL) != 0) {
        g_action_running = false;
        pthread_mutex_unlock(&g_action_mutex);
        return;
    }
    pthread_detach(thread);
    pthread_mutex_unlock(&g_action_mutex);
}

bool bluetooth_action_take(char *message, size_t message_size)
{
    pthread_mutex_lock(&g_action_mutex);
    if (!g_action_ready) {
        pthread_mutex_unlock(&g_action_mutex);
        return false;
    }
    if (message && message_size)
        snprintf(message, message_size, "%s", g_action_msg);
    g_action_ready = false;
    g_action_msg[0] = '\0';
    pthread_mutex_unlock(&g_action_mutex);
    return true;
}
