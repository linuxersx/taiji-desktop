// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "mirror.h"
#include "mirror_presets.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int store_mirror_count(void)
{
    return TAIJI_MIRROR_COUNT;
}

const char *store_mirror_label(int index)
{
    if (index < 0 || index >= TAIJI_MIRROR_COUNT) return NULL;
    return taiji_mirrors[index].label;
}

const char *store_mirror_url(int index)
{
    if (index < 0 || index >= TAIJI_MIRROR_COUNT) return NULL;
    return taiji_mirrors[index].url;
}

const char *store_mirror_id(int index)
{
    if (index < 0 || index >= TAIJI_MIRROR_COUNT) return NULL;
    return taiji_mirrors[index].id;
}

const char *store_mirror_path(void)
{
    const char *env = getenv("TAIJI_MIRRORLIST");
    return env && *env ? env : "/etc/pacman.d/mirrorlist";
}

static const char *host_path(const char *url)
{
    const char *p = url ? strstr(url, "://") : NULL;
    return p ? p + 3 : url;
}

int store_mirror_current(void)
{
    size_t len = 0;
    char *data = read_file(store_mirror_path(), &len);
    if (!data) return -1;
    int found = -1;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        line = str_trim(line);
        if (strncmp(line, "Server", 6) != 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        const char *url = str_trim(eq + 1);
        const char *hp = host_path(url);
        for (int i = 0; i < TAIJI_MIRROR_COUNT; i++) {
            const char *want = host_path(taiji_mirrors[i].url);
            size_t n = strlen(want);
            if (strncmp(hp, want, n) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) break;
    }
    free(data);
    return found;
}

static bool command_in_path(const char *cmd)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return false;
    bool ok = false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) { ok = true; break; }
    }
    free(copy);
    return ok;
}

static bool run_stream(char *const argv[], StoreProgress *progress,
                       char *error, size_t error_size)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (error) snprintf(error, error_size, "无法创建管道");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        if (error) snprintf(error, error_size, "无法启动进程");
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipefd[1]);

    char line[512];
    size_t used = 0;
    int status = 0;
    bool cancelled = false;
    for (;;) {
        if (progress && progress->cancel && *progress->cancel) {
            cancelled = true;
            kill(-pid, SIGTERM);
        }
        struct pollfd pfd = { pipefd[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) {
            ssize_t n = read(pipefd[0], line + used, sizeof(line) - used - 1);
            if (n <= 0) break;
            used += (size_t)n;
            line[used] = '\0';
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                *nl = '\0';
                if (progress && progress->progress)
                    progress->progress(progress->ud, line, -1,
                                       line[0] ? line : NULL);
                size_t left = strlen(nl + 1);
                memmove(line, nl + 1, left + 1);
                used = left;
            }
            if (used > sizeof(line) - 2) used = 0;
        } else if (pr == 0) {
            int w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                while (read(pipefd[0], line, sizeof(line)) > 0) {}
                break;
            }
        } else {
            break;
        }
    }
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    if (cancelled) {
        if (error) snprintf(error, error_size, "操作已取消");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error) snprintf(error, error_size, "更换软件源失败（退出码 %d）",
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    if (progress && progress->progress)
        progress->progress(progress->ud, "软件源已更新", 100, NULL);
    return true;
}

bool store_mirror_apply(int index, StoreProgress *progress,
                        char *error, size_t error_size)
{
    if (index < 0 || index >= TAIJI_MIRROR_COUNT) {
        if (error) snprintf(error, error_size, "未知的软件源");
        return false;
    }
    const char *helper = store_helper_path();
    char *argv[5];
    int k = 0;
    if (getenv("TAIJI_STORE_HELPER")) {
        argv[k++] = (char *)helper;
    } else if (command_in_path("pkexec")) {
        argv[k++] = "pkexec";
        argv[k++] = (char *)helper;
    } else {
        argv[k++] = (char *)helper;
    }
    argv[k++] = "pacman";
    argv[k++] = "mirror";
    argv[k++] = (char *)taiji_mirrors[index].id;
    argv[k] = NULL;
    if (progress && progress->progress)
        progress->progress(progress->ud, "正在应用软件源...", 5,
                           taiji_mirrors[index].label);
    return run_stream(argv, progress, error, error_size);
}
