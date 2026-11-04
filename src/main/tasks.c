// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "tasks.h"
#include "apps.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void copy_field(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static TaskKind kind_from_name(const char *name)
{
    if (strcmp(name, "app") == 0) return TASK_KIND_APP;
    if (strcmp(name, "music") == 0) return TASK_KIND_MUSIC;
    if (strcmp(name, "external") == 0) return TASK_KIND_EXTERNAL;
    return TASK_KIND_TAIJI;
}

static int run_helper(char *out, size_t cap)
{
    const char *env = getenv("TAIJI_TASKS_HELPER");
    const char *path = env && *env ? env : "/usr/libexec/taiji/taiji-tasks";
    if (access(path, X_OK) != 0) return -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) break;
        ssize_t n = read(fds[0], out + used, cap - 1 - used);
        if (n > 0) {
            used += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    out[used] = '\0';
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

static void add_task(TaskList *list, const char *kind, const char *id,
                     const char *title, const char *subtitle, const char *thumb,
                     long con_id, int pid, bool focused)
{
    if (list->count >= TASKS_MAX) return;
    TaskEntry *t = &list->items[list->count++];
    memset(t, 0, sizeof(*t));
    copy_field(t->kind_name, sizeof(t->kind_name), kind);
    t->kind = kind_from_name(kind);
    copy_field(t->id, sizeof(t->id), id);
    copy_field(t->title, sizeof(t->title), title && *title ? title : "未命名任务");
    copy_field(t->subtitle, sizeof(t->subtitle), subtitle);
    if (thumb && *thumb && file_exists(thumb))
        copy_field(t->thumb, sizeof(t->thumb), thumb);
    t->con_id = con_id;
    t->pid = pid;
    t->focused = focused;
}

static void parse_line(TaskList *list, char *line)
{
    if (!line || !*line) return;
    char *fields[8] = {0};
    int n = 0;
    char *p = line;
    while (n < 8) {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    if (n < 4) return;
    add_task(list,
             fields[0] ? fields[0] : "",
             fields[1] ? fields[1] : "",
             fields[2] ? fields[2] : "",
             fields[3] ? fields[3] : "",
             n > 4 ? fields[4] : NULL,
             n > 5 ? strtol(fields[5], NULL, 10) : 0,
             n > 6 ? (int)strtol(fields[6], NULL, 10) : 0,
             n > 7 ? strtol(fields[7], NULL, 10) != 0 : false);
}

static void add_fallback_tasks(TaskList *list)
{
    bool has_taiji = false;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].kind == TASK_KIND_TAIJI) { has_taiji = true; break; }
    }
    if (!has_taiji) {
        if (list->count < TASKS_MAX) {
            memmove(&list->items[1], &list->items[0],
                    sizeof(TaskEntry) * (size_t)list->count);
            list->count++;
        }
        TaskEntry *t = &list->items[0];
        memset(t, 0, sizeof(*t));
        copy_field(t->kind_name, sizeof(t->kind_name), "taiji");
        t->kind = TASK_KIND_TAIJI;
        copy_field(t->id, sizeof(t->id), "taiji");
        copy_field(t->title, sizeof(t->title), "太极");
        copy_field(t->subtitle, sizeof(t->subtitle), "主界面");
    }

    AppRuntime rt;
    apps_runtime_read(&rt);
    if (!rt.app_id[0] || !apps_runtime_alive(&rt)) return;

    /* The helper already reported this app when its window is still mapped. */
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].kind == TASK_KIND_APP &&
            strcmp(list->items[i].id, rt.app_id) == 0)
            return;
    }
    if (list->count >= TASKS_MAX) return;

    char thumb[1024];
    thumb[0] = '\0';
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime && rt.pid > 0) {
        snprintf(thumb, sizeof(thumb), "%s/taiji/thumbs/thumb-%ld.bmp",
                 runtime, (long)rt.pid);
        if (!file_exists(thumb)) {
            snprintf(thumb, sizeof(thumb), "%s/taiji/thumbs/thumb-%ld.png",
                     runtime, (long)rt.pid);
            if (!file_exists(thumb)) thumb[0] = '\0';
        }
    }
    const char *state = "已暂停";
    if (strcmp(rt.app_state, "background") == 0) state = "后台运行";
    else if (strcmp(rt.app_state, "foreground") == 0) state = "前台";
    add_task(list, "app", rt.app_id, rt.app_id, state, thumb, 0, (int)rt.pid,
             strcmp(rt.foreground, "app") == 0);
}

void tasks_refresh(TaskList *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));

    char *buf = xmalloc(65536);
    if (run_helper(buf, 65536) == 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            parse_line(list, line);
        }
    }
    free(buf);
    add_fallback_tasks(list);
}
