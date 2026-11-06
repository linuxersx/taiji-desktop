// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "files.h"
#include "config.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_restore_fullscreen = 0;

static void ensure_sway_socket(void)
{
    if (getenv("SWAYSOCK")) return;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return;
    int n = 0;
    char **items = list_dir(rt, &n);
    for (int i = 0; i < n; i++) {
        if (strncmp(items[i], "sway-ipc.", 9) != 0) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", rt, items[i]);
        if (fs_exists(p)) {
            setenv("SWAYSOCK", p, 1);
            break;
        }
    }
    free_strv(items, n);
}

static int focused_fullscreen_mode(void)
{
    FILE *p = popen("swaymsg -t get_tree 2>/dev/null | "
                    "jq -r '.. | objects | select(.focused? == true) | "
                    "select(.pid? != null and .pid > 0) | .fullscreen_mode' "
                    "2>/dev/null | head -n 1", "r");
    if (!p) return 0;
    char buf[16] = {0};
    if (!fgets(buf, sizeof(buf), p)) { pclose(p); return 0; }
    pclose(p);
    return atoi(buf);
}

static void picker_leave_caller_fullscreen(void)
{
    if (g_restore_fullscreen) return;
    ensure_sway_socket();
    if (focused_fullscreen_mode() == 1) {
        system("swaymsg fullscreen disable >/dev/null 2>&1");
        g_restore_fullscreen = 1;
    }
}

static void picker_restore_caller_fullscreen(void)
{
    if (!g_restore_fullscreen) return;
    SDL_Delay(120);
    system("swaymsg fullscreen enable >/dev/null 2>&1");
    g_restore_fullscreen = 0;
}

static void on_input(void *userdata, InputAction action, const char *text)
{
    files_ui_input((FilesApp *)userdata, action, text);
}

typedef struct {
    FilesApp *app;
    char **argv;
} OpCtx;

static void *op_thread(void *p)
{
    OpCtx *ctx = p;
    FilesApp *app = ctx->app;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(ctx->argv[0], ctx->argv);
        _exit(127);
    }
    if (pid < 0) {
        app->op_result = -1;
        app->op_done = true;
    } else {
        app->op_pid = pid;
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        app->op_result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        app->op_done = true;
    }
    for (int i = 0; ctx->argv[i]; i++) free(ctx->argv[i]);
    free(ctx->argv);
    free(ctx);
    return NULL;
}

void files_start_op(FilesApp *app, char **argv, const char *label, int action)
{
    if (app->op_running) return;
    OpCtx *ctx = xmalloc(sizeof(*ctx));
    ctx->app = app;
    int n = 0;
    while (argv[n]) n++;
    ctx->argv = xmalloc((size_t)(n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++) ctx->argv[i] = xstrdup(argv[i]);
    ctx->argv[n] = NULL;
    app->op_running = true;
    app->op_done = false;
    app->op_result = 0;
    app->op_action = action;
    snprintf(app->op_label, sizeof(app->op_label), "%s", label ? label : "正在处理...");
    files_ui_show_busy(app, app->op_label);
    pthread_t th;
    if (pthread_create(&th, NULL, op_thread, ctx) != 0) {
        app->op_running = false;
        app->op_done = true;
        app->op_result = -1;
        free(ctx->argv);
        free(ctx);
        files_ui_close_modal(app);
        snprintf(app->message, sizeof(app->message), "无法启动操作");
        return;
    }
    pthread_detach(th);
}

void files_poll_op(FilesApp *app)
{
    if (!app->op_running || !app->op_done) return;
    app->op_running = false;
    app->op_done = false;
    files_ui_close_modal(app);
    if (app->op_result != 0)
        snprintf(app->message, sizeof(app->message), "操作失败（%d）", app->op_result);
    else
        app->message[0] = '\0';
    files_ui_reload(app);
}

void files_select_path(FilesApp *app, const char *path)
{
    if (!path || !*path) return;
    if (app->output[0]) {
        FILE *f = fopen(app->output, "w");
        if (f) {
            fprintf(f, "%s\n", path);
            fclose(f);
        }
    } else {
        printf("%s\n", path);
        fflush(stdout);
    }
    app->exit_code = 0;
    app->running = false;
}

void files_cancel(FilesApp *app)
{
    app->exit_code = 1;
    app->running = false;
}

void files_finish_multi(FilesApp *app, char **paths, int count)
{
    if (count <= 0) { files_cancel(app); return; }
    if (app->output[0]) {
        FILE *f = fopen(app->output, "w");
        if (f) {
            for (int i = 0; i < count; i++) fprintf(f, "%s\n", paths[i]);
            fclose(f);
        }
    } else {
        for (int i = 0; i < count; i++) printf("%s\n", paths[i]);
        fflush(stdout);
    }
    app->exit_code = 0;
    app->running = false;
}

void files_request_output(FilesApp *app, const char *path)
{
    files_select_path(app, path);
}

int main(int argc, char **argv)
{
    Config config;
    config_init(&config);
    config_load(&config);
    i18n_set_language(config.language);
    config_free(&config);

    FilesApp app;
    memset(&app, 0, sizeof(app));
    app.mode = FM_MANAGER;
    app.focus = 0;
    app.running = true;
    app.exit_code = 0;
    app.op_pid = -1;

    const char *start = NULL;
    bool no_fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "open") == 0) app.mode = FM_OPEN;
            else if (strcmp(m, "folder") == 0) app.mode = FM_FOLDER;
            else if (strcmp(m, "save") == 0) app.mode = FM_SAVE;
            else app.mode = FM_MANAGER;
        } else if (strcmp(argv[i], "--multiple") == 0) {
            app.multiple = true;
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            snprintf(app.title, sizeof(app.title), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            snprintf(app.output, sizeof(app.output), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--filename") == 0 && i + 1 < argc) {
            snprintf(app.save_name, sizeof(app.save_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--no-fullscreen") == 0) {
            no_fullscreen = true;
        }
    }

    bool test = getenv("TAIJI_FILES_SCREENSHOT") && *getenv("TAIJI_FILES_SCREENSHOT");
    if (test || getenv("TAIJI_FILES_WINDOWED")) no_fullscreen = true;

    if (!getenv("WAYLAND_DISPLAY")) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (rt && *rt) {
            int n = 0;
            char **items = list_dir(rt, &n);
            for (int i = 0; i < n; i++) {
                const char *name = items[i];
                size_t len = strlen(name);
                if (strncmp(name, "wayland-", 8) == 0 &&
                    !(len > 5 && strcmp(name + len - 5, ".lock") == 0)) {
                    char p[PATH_MAX];
                    snprintf(p, sizeof(p), "%s/%s", rt, name);
                    if (fs_exists(p)) {
                        setenv("WAYLAND_DISPLAY", name, 1);
                        break;
                    }
                }
            }
            free_strv(items, n);
        }
    }

    if (start && *start && fs_is_dir(start))
        snprintf(app.cwd, sizeof(app.cwd), "%s", start);
    else
        snprintf(app.cwd, sizeof(app.cwd), "%s", fs_home());

    int win_w = 1280, win_h = 720;
    const char *size_env = getenv("TAIJI_FILES_SIZE");
    if (size_env && *size_env) {
        int w = 0, h = 0;
        if (sscanf(size_env, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
            win_w = w;
            win_h = h;
        }
    }
    if (!backend_init(&app.backend, win_w, win_h, on_input, &app)) {
        fprintf(stderr, "taiji-files: failed to init SDL\n");
        return 2;
    }
    backend_set_title(&app.backend, app.title[0] ? app.title : "文件管理器");

    if (!no_fullscreen) {
        if (app.mode != FM_MANAGER) picker_leave_caller_fullscreen();
        SDL_SetWindowFullscreen(app.backend.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_SetWindowAlwaysOnTop(app.backend.window, SDL_TRUE);
        SDL_SetWindowBordered(app.backend.window, SDL_FALSE);
        app.backend.fullscreen = true;
        backend_refresh_size(&app.backend);
    }

    theme_init();
    theme_set_preset(0);
    float sc = (float)app.backend.height / 720.0f;
    if (sc < 0.75f) sc = 0.75f;
    if (sc > 2.0f) sc = 2.0f;
    theme_set_scale(sc);

    files_ui_reload(&app);

    const char *test_action = getenv("TAIJI_FILES_TEST_ACTION");
    if (test_action && *test_action) {
        char *copy = xstrdup(test_action);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            if (strcmp(tok, "up") == 0) files_ui_input(&app, IN_UP, NULL);
            else if (strcmp(tok, "down") == 0) files_ui_input(&app, IN_DOWN, NULL);
            else if (strcmp(tok, "left") == 0) files_ui_input(&app, IN_LEFT, NULL);
            else if (strcmp(tok, "right") == 0) files_ui_input(&app, IN_RIGHT, NULL);
            else if (strcmp(tok, "confirm") == 0) files_ui_input(&app, IN_CONFIRM, NULL);
            else if (strcmp(tok, "start") == 0) files_ui_input(&app, IN_START, NULL);
            else if (strcmp(tok, "cancel") == 0) files_ui_input(&app, IN_CANCEL, NULL);
            else if (strcmp(tok, "x") == 0) files_ui_input(&app, IN_X, NULL);
            else if (strcmp(tok, "menu") == 0) files_ui_input(&app, IN_Y, NULL);
            else if (strncmp(tok, "text:", 5) == 0)
                files_ui_input(&app, IN_TEXT, tok + 5);
        }
        free(copy);
    }

    const char *shot = getenv("TAIJI_FILES_SCREENSHOT");
    uint32_t shot_delay = 1200;
    const char *sd = getenv("TAIJI_FILES_SCREENSHOT_DELAY");
    if (sd && *sd) shot_delay = (uint32_t)atoi(sd);
    uint32_t t0 = SDL_GetTicks();

    while (app.running) {
        backend_poll(&app.backend);
        files_poll_op(&app);
        if (app.backend.resized) {
            app.backend.resized = false;
            float s2 = (float)app.backend.height / 720.0f;
            if (s2 < 0.75f) s2 = 0.75f;
            if (s2 > 2.0f) s2 = 2.0f;
            theme_set_scale(s2);
            files_ui_reload(&app);
        }
        files_ui_tick(&app);
        if (!app.running) break;
        lv_timer_handler();
        backend_present(&app.backend);
        if (shot && *shot && SDL_GetTicks() - t0 > shot_delay) {
            backend_save_bmp(&app.backend, shot);
            app.running = false;
        }
    }

    files_ui_free(&app);
    backend_shutdown(&app.backend);
    picker_restore_caller_fullscreen();
    fs_list_free(&app.list);
    return app.exit_code;
}
