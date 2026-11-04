// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "external_osk.h"
#include "ui.h"
#include "util.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

static void dlog(const char *fmt, ...)
{
    const char *path = getenv("ARMDECK_OSK_LOG");
    if (!path || !*path) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static pid_t g_spawn_pid;
static time_t g_spawn_cooldown;
static time_t g_next_connect;
static time_t g_next_show_retry;
static bool g_want_show;
static uint32_t g_theme_sig;

static bool target_store_any(App *app);
static bool target_modal_any(App *app);

static const char *osk_bin(void)
{
    const char *env = getenv("ARMDECK_OSK_BIN");
    return env && *env ? env : "/usr/bin/armdeck-osk";
}

static void osk_runtime_path(const char *suffix, char *out, size_t n)
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
    snprintf(out, n, "%s/armdeck-osk/osk-%s.%s",
             runtime && *runtime ? runtime : "/tmp", safe, suffix);
}

static void osk_socket_path(char *out, size_t n)
{
    osk_runtime_path("sock", out, n);
}

static bool osk_visible(void)
{
    char path[700];
    osk_runtime_path("visible", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char c = 0;
    if (fread(&c, 1, 1, f) != 1) c = 0;
    fclose(f);
    return c == '1';
}

bool external_osk_available(void)
{
    return access(osk_bin(), X_OK) == 0;
}

static bool osk_send(App *app, const char *line)
{
    if (app->ext_osk_fd < 0) return false;
    ssize_t n = send(app->ext_osk_fd, line, strlen(line), MSG_NOSIGNAL);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        external_osk_close(app);
        return false;
    }
    return n == (ssize_t)strlen(line);
}

static bool osk_connect(App *app)
{
    if (app->ext_osk_fd >= 0) return true;
    char path[600];
    osk_socket_path(path, sizeof(path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    app->ext_osk_fd = fd;
    app->ext_osk_attached = false;
    app->ext_osk_len = 0;
    return true;
}

static void osk_reap(void)
{
    if (g_spawn_pid <= 0) return;
    int status = 0;
    pid_t r = waitpid(g_spawn_pid, &status, WNOHANG);
    if (r == g_spawn_pid) {
        g_spawn_pid = 0;
        g_spawn_cooldown = time(NULL) + 5;
    }
}

static void osk_spawn(void)
{
    const char *bin = getenv("ARMDECK_OSK_BIN");
    if (!bin || !*bin) {
        bin = "/usr/bin/armdeck-oskd";
        if (access(bin, X_OK) != 0) bin = osk_bin();
    }
    if (access(bin, X_OK) != 0) return;
    if (g_spawn_pid > 0) return;
    if (time(NULL) < g_spawn_cooldown) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > 2) close(nullfd);
        }
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) g_spawn_pid = pid;
}

static uint32_t osk_theme_sig(void)
{
    uint32_t h = 2166136261u;
    const uint32_t vals[] = {
        PAL.bg, PAL.surface, PAL.card, PAL.card_soft, PAL.elevated,
        PAL.text, PAL.text2, PAL.text3, PAL.accent, PAL.accent2,
        PAL.border, PAL.border_focus,
    };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
        h = (h ^ vals[i]) * 16777619u;
    h = (h ^ (PAL.is_light ? 1u : 0u)) * 16777619u;
    return h ? h : 1u;
}

/* Send the current desktop palette so the keyboard follows the app theme.
 * The keyboard can ignore it when its own config sets lock_theme=1. */
static void osk_push_theme(App *app)
{
    if (app->ext_osk_fd < 0 || !app->ext_osk_attached) return;
    uint32_t sig = osk_theme_sig();
    if (sig == g_theme_sig) return;
    char line[512];
    snprintf(line, sizeof(line),
             "theme %d %08X %08X %08X %08X %08X %08X %08X %08X "
             "%08X %08X %08X %08X %d\n",
             app->active_theme, PAL.bg, PAL.surface, PAL.card, PAL.card_soft,
             PAL.elevated, PAL.text, PAL.text2, PAL.text3, PAL.accent,
             PAL.accent2, PAL.border, PAL.border_focus, PAL.is_light ? 1 : 0);
    if (osk_send(app, line)) g_theme_sig = sig;
}

static bool osk_attach_show(App *app)
{
    if (!app->ext_osk_attached) {
        if (!osk_send(app, "attach\n")) return false;
        app->ext_osk_attached = true;
        g_theme_sig = 0;
    }
    osk_push_theme(app);
    if (!osk_send(app, "show\n")) return false;
    return true;
}

bool external_osk_show(App *app)
{
    if (!external_osk_available()) return false;
    g_want_show = true;
    if (app->ext_osk_fd < 0) {
        time_t now = time(NULL);
        if (now < g_next_connect) return false;
        g_next_connect = now + 1;
        if (!osk_connect(app)) osk_spawn();
    }
    bool ok = osk_attach_show(app);
    dlog("show fd=%d ok=%d store=%d modal=%d visible=%d", app->ext_osk_fd, ok,
         target_store_any(app), target_modal_any(app), osk_visible());
    return ok;
}

void external_osk_hide(App *app)
{
    g_want_show = false;
    if (app->ext_osk_fd >= 0) osk_send(app, "hide\n");
}

void external_osk_close(App *app)
{
    if (app->ext_osk_fd >= 0) close(app->ext_osk_fd);
    app->ext_osk_fd = -1;
    app->ext_osk_attached = false;
    app->ext_osk_len = 0;
    g_want_show = false;
}

static void utf8_backspace(char *s)
{
    if (!s || !*s) return;
    size_t n = strlen(s);
    if (n == 0) return;
    n--;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = 0;
}

static void append_text(char *dst, size_t n, const char *text)
{
    if (!dst || !n || !text) return;
    size_t used = strlen(dst);
    if (used >= n - 1) return;
    snprintf(dst + used, n - used, "%s", text);
}

static bool target_store(App *app)
{
    return app->screen == SCREEN_STORE && app->store_search_edit;
}

static bool target_modal(App *app)
{
    return app->modal == MODAL_TEXT || app->modal == MODAL_SEARCH;
}

static bool target_store_any(App *app) { return target_store(app); }
static bool target_modal_any(App *app) { return target_modal(app); }

static void refresh_target(App *app, bool store)
{
    if (store) {
        if (app->screen == SCREEN_STORE) app_rebuild(app);
    } else {
        ui_refresh_modal(app);
    }
}

static void apply_text(App *app, const char *text)
{
    if (target_store(app)) {
        append_text(app->store_search, sizeof(app->store_search), text);
        refresh_target(app, true);
    } else if (app->modal == MODAL_SEARCH) {
        append_text(app->search_buf, sizeof(app->search_buf), text);
        app->search_focus = 0;
        ui_refresh_modal(app);
    } else if (app->modal == MODAL_TEXT) {
        append_text(app->input_buf, sizeof(app->input_buf), text);
        app->input_cursor = (int)strlen(app->input_buf);
        ui_refresh_modal(app);
    }
}

static void apply_clear(App *app)
{
    if (target_store(app)) {
        app->store_search[0] = '\0';
        refresh_target(app, true);
    } else if (app->modal == MODAL_SEARCH) {
        app->search_buf[0] = '\0';
        app->search_focus = 0;
        ui_refresh_modal(app);
    } else if (app->modal == MODAL_TEXT) {
        app->input_buf[0] = '\0';
        app->input_cursor = 0;
        ui_refresh_modal(app);
    }
}

static void apply_backspace(App *app)
{
    if (target_store(app)) {
        utf8_backspace(app->store_search);
        refresh_target(app, true);
    } else if (app->modal == MODAL_SEARCH) {
        utf8_backspace(app->search_buf);
        app->search_focus = 0;
        ui_refresh_modal(app);
    } else if (app->modal == MODAL_TEXT) {
        utf8_backspace(app->input_buf);
        app->input_cursor = (int)strlen(app->input_buf);
        ui_refresh_modal(app);
    }
}

void external_osk_poll(App *app)
{
    osk_reap();
    osk_push_theme(app);
    {
        static int last = -1;
        int cur = (target_store_any(app) ? 1 : 0) | (target_modal_any(app) ? 2 : 0);
        if (cur != last) {
            last = cur;
            dlog("target store=%d modal=%d visible=%d fd=%d",
                 target_store_any(app), target_modal_any(app), osk_visible(),
                 app->ext_osk_fd);
        }
    }
    /* 用户点“收起”后，如果新的输入框已经打开但 OSK 没收到 show，
     * 由主循环补发一次。visible 由 armdeck-osk 维护。 */
    if ((target_store(app) || target_modal(app)) && !osk_visible()) {
        time_t now = time(NULL);
        if (now >= g_next_show_retry) {
            g_next_show_retry = now + 1;
            dlog("retry show");
            external_osk_show(app);
        }
    }
    /* 0/1/2 是进程标准流，不能当作 OSK socket。 */
    if (app->ext_osk_fd >= 0 && app->ext_osk_fd <= 2) {
        app->ext_osk_fd = -1;
        app->ext_osk_attached = false;
        app->ext_osk_len = 0;
    }
    if (app->ext_osk_fd < 0) {
        if (external_osk_available() && (target_store(app) || target_modal(app))) {
            time_t now = time(NULL);
            if (now >= g_next_connect) {
                g_next_connect = now + 1;
                if (osk_connect(app)) osk_attach_show(app);
                else osk_spawn();
            }
        }
        if (app->ext_osk_fd < 0) return;
    }
    char buf[512];
    for (;;) {
        ssize_t n = read(app->ext_osk_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            external_osk_close(app);
            return;
        }
        if (n == 0) { external_osk_close(app); return; }
        if (app->ext_osk_len + (size_t)n >= sizeof(app->ext_osk_buf))
            app->ext_osk_len = 0;
        memcpy(app->ext_osk_buf + app->ext_osk_len, buf, (size_t)n);
        app->ext_osk_len += (size_t)n;
        app->ext_osk_buf[app->ext_osk_len] = 0;
        char *start = app->ext_osk_buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            char *line = start;
            if (strncmp(line, "TEXT ", 5) == 0) { dlog("recv TEXT [%s]", line + 5); apply_text(app, line + 5); }
            else if (!strcmp(line, "BACKSPACE")) apply_backspace(app);
            else if (!strcmp(line, "CLEAR")) apply_clear(app);
            else if (!strcmp(line, "ENTER")) app_input(app, IN_CONFIRM, NULL);
            else if (!strcmp(line, "CANCEL")) { dlog("recv CANCEL"); app_input(app, IN_CANCEL, NULL); }
            start = nl + 1;
        }
        size_t left = strlen(start);
        memmove(app->ext_osk_buf, start, left + 1);
        app->ext_osk_len = left;
    }
}
