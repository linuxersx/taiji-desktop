// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "files.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int entry_cmp(const void *a, const void *b)
{
    const FsEntry *x = a;
    const FsEntry *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

bool fs_list_dir(const char *path, bool hidden, FsList *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    DIR *d = opendir(path);
    if (!d) return false;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!hidden && e->d_name[0] == '.') continue;
        char full[PATH_MAX];
        fs_join(full, sizeof(full), path, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        /* 符号链接按目标类型显示：指向目录的链接仍然当作文件夹。 */
        bool is_dir = S_ISDIR(st.st_mode);
        if (S_ISLNK(st.st_mode)) {
            struct stat target;
            if (stat(full, &target) == 0) {
                is_dir = S_ISDIR(target.st_mode);
                st = target;
            }
        }
        if (out->count == cap) {
            cap = cap ? cap * 2 : 64;
            out->items = xrealloc(out->items, (size_t)cap * sizeof(FsEntry));
        }
        FsEntry *it = &out->items[out->count++];
        memset(it, 0, sizeof(*it));
        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        it->is_dir = is_dir;
        it->size = st.st_size;
        it->mtime = st.st_mtime;
    }
    closedir(d);
    qsort(out->items, (size_t)out->count, sizeof(FsEntry), entry_cmp);
    return true;
}

void fs_list_free(FsList *list)
{
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void fs_join(char *out, size_t n, const char *a, const char *b)
{
    if (!out || n == 0) return;
    if (!a || !*a) a = "/";
    if (!b || !*b) { snprintf(out, n, "%s", a); return; }
    if (*b == '/') { snprintf(out, n, "%s", b); return; }
    size_t len = strlen(a);
    if (len > 0 && a[len - 1] == '/')
        snprintf(out, n, "%s%s", a, b);
    else
        snprintf(out, n, "%s/%s", a, b);
}

bool fs_parent(const char *path, char *out, size_t n)
{
    if (!path || !*path) return false;
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';
    if (strcmp(buf, "/") == 0) return false;
    char *slash = strrchr(buf, '/');
    if (!slash) return false;
    if (slash == buf) {
        snprintf(out, n, "/");
    } else {
        *slash = '\0';
        snprintf(out, n, "%s", buf);
    }
    return true;
}

bool fs_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool fs_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void fs_format_size(off_t size, char *out, size_t n)
{
    if (size < 1024) snprintf(out, n, "%lld B", (long long)size);
    else if (size < 1024 * 1024) snprintf(out, n, "%.1f KB", (double)size / 1024.0);
    else if (size < (off_t)1024 * 1024 * 1024)
        snprintf(out, n, "%.1f MB", (double)size / (1024.0 * 1024.0));
    else snprintf(out, n, "%.2f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
}

void fs_format_time(time_t t, char *out, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M", &tm);
}

const char *fs_home(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : "/";
}

static bool add_shortcut(char paths[][PATH_MAX], char labels[][64], int *count,
                         int max, const char *path, const char *label)
{
    if (*count >= max || !path || !*path || !fs_is_dir(path)) return false;
    for (int i = 0; i < *count; i++) {
        if (strcmp(paths[i], path) == 0) return false;
    }
    snprintf(paths[*count], PATH_MAX, "%s", path);
    snprintf(labels[*count], 64, "%s", label);
    (*count)++;
    return true;
}

int fs_shortcuts(char paths[][PATH_MAX], char labels[][64], int max)
{
    int n = 0;
    const char *home = fs_home();
    add_shortcut(paths, labels, &n, max, home, "主目录");
    char tmp[PATH_MAX];
    fs_join(tmp, sizeof(tmp), home, "ROMs");
    add_shortcut(paths, labels, &n, max, tmp, "ROMs");
    fs_join(tmp, sizeof(tmp), home, "Downloads");
    add_shortcut(paths, labels, &n, max, tmp, "下载");
    add_shortcut(paths, labels, &n, max, "/", "根目录");
    add_shortcut(paths, labels, &n, max, "/mnt", "挂载 /mnt");
    add_shortcut(paths, labels, &n, max, "/media", "挂载 /media");

    const char *user = getenv("USER");
    if (user && *user) {
        snprintf(tmp, sizeof(tmp), "/run/media/%s", user);
        DIR *d = opendir(tmp);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < max) {
                if (e->d_name[0] == '.') continue;
                char p[PATH_MAX];
                fs_join(p, sizeof(p), tmp, e->d_name);
                char label[96];
                snprintf(label, sizeof(label), "U 盘 %s", e->d_name);
                add_shortcut(paths, labels, &n, max, p, label);
            }
            closedir(d);
        }
    }
    return n;
}
