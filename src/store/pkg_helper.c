// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mirror_presets.h"
#include "flatpak_presets.h"

static bool valid_name(const char *s)
{
    if (!s || !*s || strlen(s) > 128) return false;
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '@' || *p == '.' ||
              *p == '_' || *p == '+' || *p == '-'))
            return false;
    }
    return true;
}

static bool protected_name(const char *id)
{
    /* 固件绝对禁止；内核和启动相关包也默认保护。 */
    static const char *exact[] = {
        "linux", "linux-lts", "linux-zen", "linux-hardened",
        "linux-firmware", "linux-firmware-whence", "linux-api-headers",
        "alsa-firmware", "sof-firmware", "arm-trusted-firmware",
        "u-boot", "uboot", "grub", "systemd-boot", "edk2-ovmf"
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (strcmp(id, exact[i]) == 0) return true;
    static const char *prefix[] = {
        "linux", "firmware", "u-boot", "uboot", "lk-", "atf-", "boot", "grub"
    };
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
        if (strncmp(id, prefix[i], strlen(prefix[i])) == 0) return true;
    if (strstr(id, "firmware")) return true;
    return false;
}

static void protected_ignore_list(char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *f = popen("LC_ALL=C pacman -Qq 2>/dev/null", "r");
    if (!f) return;
    char name[256];
    size_t used = 0;
    while (fgets(name, sizeof(name), f)) {
        name[strcspn(name, "\r\n")] = '\0';
        if (!name[0] || !protected_name(name)) continue;
        int need = snprintf(NULL, 0, "%s%s", used ? "," : "", name);
        if (used + (size_t)need + 1 >= out_size) break;
        used += (size_t)snprintf(out + used, out_size - used, "%s%s",
                                 used ? "," : "", name);
    }
    pclose(f);
}

static void log_line(const char *text)
{
    int fd = open("/var/log/taiji-store.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    dprintf(fd, "%s\n", text);
    close(fd);
}

static bool valid_flatpak_id(const char *s)
{
    if (!s || !*s || strlen(s) > 255) return false;
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

static int run_flatpak(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp("flatpak", argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int flatpak_remote_apply(const char *id)
{
    const TaijiFlatpakPreset *preset = NULL;
    for (int i = 0; i < TAIJI_FLATPAK_MIRROR_COUNT; i++) {
        if (strcmp(taiji_flatpak_mirrors[i].id, id) == 0) {
            preset = &taiji_flatpak_mirrors[i];
            break;
        }
    }
    if (!preset) {
        fprintf(stderr, "未知的 Flatpak 软件源: %s\n", id);
        return 2;
    }
    char url_arg[1024];
    snprintf(url_arg, sizeof(url_arg), "--url=%s", preset->url);
    char *modify[] = {"flatpak", "remote-modify", "--system", url_arg,
                      "flathub", NULL};
    if (run_flatpak(modify) == 0) {
        char *refresh[] = {"flatpak", "update", "--system", "--appstream", NULL};
        run_flatpak(refresh);
        return 0;
    }
    char *add[] = {"flatpak", "remote-add", "--system", "--if-not-exists",
                   "--gpg-import=/usr/share/flatpak/flathub.gpg",
                   "flathub", (char *)preset->url, NULL};
    int rc = run_flatpak(add);
    if (rc != 0) {
        char *add_repo[] = {"flatpak", "remote-add", "--system", "--if-not-exists",
                            "flathub",
                            "https://dl.flathub.org/repo/flathub.flatpakrepo",
                            NULL};
        rc = run_flatpak(add_repo);
    }
    if (rc != 0) return rc;
    run_flatpak(modify);
    char *refresh[] = {"flatpak", "update", "--system", "--appstream", NULL};
    run_flatpak(refresh);
    return 0;
}

static int flatpak_helper(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: taiji-store-helper flatpak <install|remove|update|remote> ...\n");
        return 2;
    }
    const char *op = argv[2];
    int count = argc - 3;
    if (count > 32) {
        fprintf(stderr, "一次最多处理 32 个应用\n");
        return 2;
    }
    for (int i = 0; i < count; i++) {
        if (!valid_flatpak_id(argv[3 + i])) {
            fprintf(stderr, "非法 Flatpak ID: %s\n", argv[3 + i]);
            return 2;
        }
    }
    if (strcmp(op, "remote") == 0) {
        if (count != 1) {
            fprintf(stderr, "remote 需要一个软件源 ID\n");
            return 2;
        }
        return flatpak_remote_apply(argv[3]);
    }
    char **args = calloc((size_t)count + 10, sizeof(char *));
    int k = 0;
    args[k++] = "flatpak";
    if (strcmp(op, "install") == 0) {
        args[k++] = "install";
        args[k++] = "--system";
        args[k++] = "--noninteractive";
        args[k++] = "-y";
        args[k++] = "flathub";
        for (int i = 0; i < count; i++) args[k++] = argv[3 + i];
    } else if (strcmp(op, "remove") == 0) {
        args[k++] = "uninstall";
        args[k++] = "--system";
        args[k++] = "--noninteractive";
        args[k++] = "-y";
        for (int i = 0; i < count; i++) args[k++] = argv[3 + i];
    } else if (strcmp(op, "update") == 0) {
        args[k++] = "update";
        args[k++] = "--system";
        args[k++] = "--noninteractive";
        args[k++] = "-y";
    } else {
        fprintf(stderr, "未知 Flatpak 操作: %s\n", op);
        free(args);
        return 2;
    }
    args[k] = NULL;
    char logbuf[512];
    snprintf(logbuf, sizeof(logbuf), "op=flatpak-%s count=%d", op, count);
    log_line(logbuf);
    execvp(args[0], args);
    fprintf(stderr, "无法执行 flatpak: %s\n", strerror(errno));
    free(args);
    return 1;
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        fprintf(stderr, "需要 root 权限\n");
        return 1;
    }
    if (argc < 3 || (strcmp(argv[1], "pacman") != 0 &&
                     strcmp(argv[1], "flatpak") != 0)) {
        fprintf(stderr, "用法: taiji-store-helper <pacman|flatpak> <install|remove|update> ...\n");
        return 2;
    }
    if (strcmp(argv[1], "flatpak") == 0)
        return flatpak_helper(argc, argv);
    const char *op = argv[2];
    int count = argc - 3;
    if (count > 32) {
        fprintf(stderr, "一次最多处理 32 个包\n");
        return 2;
    }
    for (int i = 0; i < count; i++) {
        if (!valid_name(argv[3 + i])) {
            fprintf(stderr, "非法包名: %s\n", argv[3 + i]);
            return 2;
        }
        if ((strcmp(op, "remove") == 0 || strcmp(op, "install") == 0) &&
            protected_name(argv[3 + i])) {
            fprintf(stderr, "受保护的包不能通过商店修改: %s\n", argv[3 + i]);
            return 3;
        }
    }

    char logbuf[512];
    snprintf(logbuf, sizeof(logbuf), "op=%s count=%d", op, count);
    log_line(logbuf);

    char **args = calloc((size_t)count + 8, sizeof(char *));
    int k = 0;
    if (strcmp(op, "mirror") == 0) {
        if (count != 1) {
            fprintf(stderr, "mirror 需要一个软件源 ID\n");
            free(args);
            return 2;
        }
        const TaijiMirrorPreset *preset = NULL;
        for (int i = 0; i < TAIJI_MIRROR_COUNT; i++)
            if (strcmp(taiji_mirrors[i].id, argv[3]) == 0) {
                preset = &taiji_mirrors[i];
                break;
            }
        if (!preset) {
            fprintf(stderr, "未知的软件源: %s\n", argv[3]);
            free(args);
            return 2;
        }
        const char *path_env = getenv("TAIJI_MIRRORLIST");
        const char *path = path_env && *path_env ? path_env
                                                 : "/etc/pacman.d/mirrorlist";
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s.taiji.tmp", path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "无法写入软件源: %s\n", strerror(errno));
            free(args);
            return 1;
        }
        dprintf(fd, "# Generated by Taiji\nServer = %s\n", preset->url);
        close(fd);
        if (rename(tmp, path) != 0) {
            unlink(tmp);
            fprintf(stderr, "无法保存软件源: %s\n", strerror(errno));
            free(args);
            return 1;
        }
        snprintf(logbuf, sizeof(logbuf), "op=mirror id=%s", preset->id);
        log_line(logbuf);
        args[k++] = "pacman";
        args[k++] = "-Syy";
        args[k++] = "--noconfirm";
        args[k] = NULL;
        execvp(args[0], args);
        fprintf(stderr, "无法执行 pacman: %s\n", strerror(errno));
        free(args);
        return 1;
    }
    args[k++] = "pacman";
    if (strcmp(op, "install") == 0) {
        args[k++] = "-S";
        args[k++] = "--needed";
        args[k++] = "--noconfirm";
        args[k++] = "--";
        for (int i = 0; i < count; i++) args[k++] = argv[3 + i];
    } else if (strcmp(op, "remove") == 0) {
        args[k++] = "-Rns";
        args[k++] = "--noconfirm";
        args[k++] = "--";
        for (int i = 0; i < count; i++) args[k++] = argv[3 + i];
    } else if (strcmp(op, "update") == 0) {
        static char ignore[8192];
        protected_ignore_list(ignore, sizeof(ignore));
        args[k++] = "-Syu";
        args[k++] = "--noconfirm";
        if (ignore[0]) {
            args[k++] = "--ignore";
            args[k++] = ignore;
        }
    } else {
        fprintf(stderr, "未知操作: %s\n", op);
        free(args);
        return 2;
    }
    args[k] = NULL;

    execvp(args[0], args);
    fprintf(stderr, "无法执行 pacman: %s\n", strerror(errno));
    free(args);
    return 1;
}
