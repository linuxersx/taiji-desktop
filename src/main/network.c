// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "network.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#endif

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

static bool read_cmd(const char *cmd, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool got = false;
    if (fgets(out, (int)out_size, f)) {
        out[strcspn(out, "\r\n")] = '\0';
        got = true;
    }
    int rc = pclose(f);
    if (rc != 0) out[0] = '\0';
    return got && rc == 0;
}

/* Simpler escaped-colon splitter used for nmcli -t output. */
#ifdef __linux__
static int split_terse(char *line, char **fields, int max)
{
    int n = 0;
    char *w = line;
    char *start = line;
    for (char *p = line; *p; ) {
        if (*p == '\\' && p[1]) {
            p++;
            *w++ = *p++;
        } else if (*p == ':' && n + 1 < max) {
            *w++ = '\0';
            fields[n++] = start;
            start = w;
            p++;
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
    fields[n++] = start;
    return n;
}

static int cmp_network(const void *a, const void *b)
{
    const WifiNetwork *x = a, *y = b;
    if (x->active != y->active) return x->active ? -1 : 1;
    return y->signal - x->signal;
}
#endif

#ifdef __linux__
static void scan_linux(WifiState *st, bool rescan)
{
    st->available = command_exists("nmcli");
    if (!st->available) {
        snprintf(st->message, sizeof(st->message), "未找到 nmcli / NetworkManager");
        st->enabled = false;
        return;
    }

    char buf[128];
    if (read_cmd("LC_ALL=C nmcli -t -f WIFI general 2>/dev/null", buf, sizeof(buf))) {
        st->enabled = strstr(buf, "enabled") != NULL;
    }
    if (rescan) {
        run_status("LC_ALL=C nmcli dev wifi rescan >/dev/null 2>&1");
    }

    FILE *f = popen("LC_ALL=C nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null", "r");
    if (!f) {
        snprintf(st->message, sizeof(st->message), "无法读取 NetworkManager 网络列表");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char *fields[5] = {0};
        int n = split_terse(line, fields, 4);
        if (n < 4) continue;
        const char *inuse = fields[0];
        const char *ssid = fields[1];
        const char *sig = fields[2];
        const char *sec = fields[3];
        if (!ssid || !*ssid) continue;
        int signal = atoi(sig);
        bool active = inuse && strchr(inuse, '*') != NULL;
        bool secure = sec && *sec && strcmp(sec, "--") != 0;
        int found = -1;
        for (int i = 0; i < st->count; i++) {
            if (strcmp(st->items[i].ssid, ssid) == 0) { found = i; break; }
        }
        if (found >= 0) {
            if (active || signal > st->items[found].signal) {
                st->items[found].active = st->items[found].active || active;
                st->items[found].signal = signal;
                snprintf(st->items[found].security, sizeof(st->items[found].security), "%s", sec ? sec : "");
                st->items[found].secure = secure;
            }
        } else if (st->count < 64) {
            WifiNetwork *nw = &st->items[st->count++];
            memset(nw, 0, sizeof(*nw));
            snprintf(nw->ssid, sizeof(nw->ssid), "%s", ssid);
            snprintf(nw->security, sizeof(nw->security), "%s", sec ? sec : "");
            nw->signal = signal;
            nw->active = active;
            nw->secure = secure;
        }
        if (active) {
            snprintf(st->current_ssid, sizeof(st->current_ssid), "%s", ssid);
            st->current_signal = signal;
        }
    }
    pclose(f);
    qsort(st->items, (size_t)st->count, sizeof(st->items[0]), cmp_network);
    if (!st->message[0]) {
        if (st->count == 0) snprintf(st->message, sizeof(st->message), "没有扫描到 Wi-Fi 网络");
        else snprintf(st->message, sizeof(st->message), "NetworkManager");
    }
}
#endif

#ifdef __APPLE__
static void scan_macos(WifiState *st)
{
    st->available = true;
    st->enabled = true;
    char buf[256];
    if (read_cmd("networksetup -getairportpower en0 2>/dev/null", buf, sizeof(buf))) {
        st->enabled = strstr(buf, "On") != NULL;
    }
    if (read_cmd("networksetup -getairportnetwork en0 2>/dev/null", buf, sizeof(buf))) {
        char *colon = strchr(buf, ':');
        if (colon && !strstr(buf, "not associated") && !strstr(buf, "not a Wi-Fi")) {
            snprintf(st->current_ssid, sizeof(st->current_ssid), "%s", colon + 1);
            char *s = str_trim(st->current_ssid);
            if (s != st->current_ssid) memmove(st->current_ssid, s, strlen(s) + 1);
            st->current_signal = 0;
        }
    }
    snprintf(st->message, sizeof(st->message), "macOS 系统 Wi-Fi");
}
#endif

void network_wifi_scan(WifiState *st, bool rescan)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
#ifdef __linux__
    scan_linux(st, rescan);
#elif defined(__APPLE__)
    (void)rescan;
    scan_macos(st);
#else
    snprintf(st->message, sizeof(st->message), "当前系统不支持 Wi-Fi 管理");
#endif
}

static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static WifiState g_scan_state;
static bool g_scan_running;
static bool g_scan_ready;
static bool g_scan_rescan;

static void *wifi_scan_worker(void *arg)
{
    (void)arg;
    WifiState result;
    network_wifi_scan(&result, g_scan_rescan);
    pthread_mutex_lock(&g_scan_mutex);
    g_scan_state = result;
    g_scan_ready = true;
    g_scan_running = false;
    pthread_mutex_unlock(&g_scan_mutex);
    return NULL;
}

void network_wifi_scan_async(bool rescan)
{
    pthread_mutex_lock(&g_scan_mutex);
    if (g_scan_running) {
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }
    g_scan_running = true;
    g_scan_ready = false;
    g_scan_rescan = rescan;
    pthread_t thread;
    if (pthread_create(&thread, NULL, wifi_scan_worker, NULL) != 0) {
        g_scan_running = false;
        pthread_mutex_unlock(&g_scan_mutex);
        return;
    }
    pthread_detach(thread);
    pthread_mutex_unlock(&g_scan_mutex);
}

bool network_wifi_scan_take(WifiState *state)
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

/* Fast radio-state check used when the panel opens, before the async scan. */
bool network_wifi_radio(bool *enabled, bool *available)
{
    bool av = false, en = false;
#ifdef __linux__
    av = command_exists("nmcli");
    if (av) {
        char buf[128];
        if (read_cmd("LC_ALL=C nmcli -t -f WIFI general 2>/dev/null", buf, sizeof(buf))) {
            en = strstr(buf, "enabled") != NULL;
        }
    }
#elif defined(__APPLE__)
    av = true;
    char buf[256];
    if (read_cmd("networksetup -getairportpower en0 2>/dev/null", buf, sizeof(buf))) {
        en = strstr(buf, "On") != NULL;
    }
#else
    av = false;
#endif
    if (enabled) *enabled = en;
    if (available) *available = av;
    return av;
}

bool network_wifi_toggle(bool enabled, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
#ifdef __linux__
    if (!command_exists("nmcli")) {
        if (error) snprintf(error, error_size, "未找到 nmcli");
        return false;
    }
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "LC_ALL=C nmcli radio wifi %s >/dev/null 2>&1",
             enabled ? "on" : "off");
    int rc = run_status(cmd);
    if (rc != 0 && error) snprintf(error, error_size, "NetworkManager 切换 Wi-Fi 失败");
    return rc == 0;
#elif defined(__APPLE__)
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "networksetup -setairportpower en0 %s >/dev/null 2>&1",
             enabled ? "on" : "off");
    int rc = run_status(cmd);
    if (rc != 0 && error) snprintf(error, error_size, "macOS 切换 Wi-Fi 失败");
    return rc == 0;
#else
    if (error) snprintf(error, error_size, "当前系统不支持 Wi-Fi 管理");
    return false;
#endif
}

bool network_wifi_connect(const char *ssid, const char *password,
                          char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!ssid || !*ssid) {
        if (error) snprintf(error, error_size, "SSID 为空");
        return false;
    }
#ifdef __linux__
    if (!command_exists("nmcli")) {
        if (error) snprintf(error, error_size, "未找到 nmcli");
        return false;
    }
    char *qssid = shell_quote(ssid);
    char cmd[2048];
    if (password && *password) {
        char *qpass = shell_quote(password);
        snprintf(cmd, sizeof(cmd),
                 "LC_ALL=C nmcli --wait 20 dev wifi connect %s password %s 2>&1",
                 qssid, qpass);
        free(qpass);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "LC_ALL=C nmcli --wait 20 dev wifi connect %s 2>&1", qssid);
    }
    free(qssid);
    int rc = run_status(cmd);
    if (rc != 0 && error) snprintf(error, error_size, "连接失败，请检查密码或网络");
    return rc == 0;
#elif defined(__APPLE__)
    (void)password;
    if (error) snprintf(error, error_size, "请在 macOS 系统设置中管理 Wi-Fi");
    return false;
#else
    (void)password;
    if (error) snprintf(error, error_size, "当前系统不支持 Wi-Fi 管理");
    return false;
#endif
}
