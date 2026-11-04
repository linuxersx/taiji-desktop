// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "icon_index.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool has_suffix_ci(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcasecmp(s + sl - fl, suffix) == 0;
}

static void lower_copy(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (; in && *in && k + 1 < n; in++) out[k++] = (char)tolower((unsigned char)*in);
    out[k] = '\0';
}

static uint32_t icon_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static void slots_rehash(IconIndex *idx, int cap)
{
    int *slots = calloc((size_t)cap, sizeof(int));
    if (!slots) return;
    for (int i = 0; i < idx->icon_count; i++) {
        uint32_t h = icon_hash(idx->icons[i].name) & (uint32_t)(cap - 1);
        while (slots[h]) h = (h + 1) & (uint32_t)(cap - 1);
        slots[h] = i + 1;
    }
    free(idx->slots);
    idx->slots = slots;
    idx->slot_cap = cap;
}

/* Returns the index of an existing name, or -1. */
static int icon_find(IconIndex *idx, const char *key)
{
    if (!idx->slots || idx->slot_cap == 0) return -1;
    uint32_t mask = (uint32_t)(idx->slot_cap - 1);
    uint32_t h = icon_hash(key) & mask;
    while (idx->slots[h]) {
        int i = idx->slots[h] - 1;
        if (strcmp(idx->icons[i].name, key) == 0) return i;
        h = (h + 1) & mask;
    }
    return -1;
}

static void add_icon(IconIndex *idx, const char *name, const char *path, int rank)
{
    if (!name || !*name) return;
    char key[160];
    lower_copy(key, sizeof(key), name);
    int found = icon_find(idx, key);
    if (found >= 0) {
        if (rank < idx->icons[found].rank) {
            idx->icons[found].rank = rank;
            snprintf(idx->icons[found].path, sizeof(idx->icons[found].path), "%s", path);
        }
        return;
    }
    if (idx->icon_count == idx->icon_cap) {
        int cap = idx->icon_cap ? idx->icon_cap * 2 : 512;
        idx->icons = realloc(idx->icons, (size_t)cap * sizeof(IconEntry));
        idx->icon_cap = cap;
    }
    if (idx->slot_cap == 0 || (idx->icon_count + 1) * 10 >= idx->slot_cap * 7) {
        int cap = idx->slot_cap ? idx->slot_cap * 2 : 4096;
        slots_rehash(idx, cap);
    }
    IconEntry *e = &idx->icons[idx->icon_count];
    snprintf(e->name, sizeof(e->name), "%s", key);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->rank = rank;
    if (idx->slots) {
        uint32_t mask = (uint32_t)(idx->slot_cap - 1);
        uint32_t h = icon_hash(key) & mask;
        while (idx->slots[h]) h = (h + 1) & mask;
        idx->slots[h] = idx->icon_count + 1;
    }
    idx->icon_count++;
}

static void add_desktop(IconIndex *idx, const char *key, const char *icon)
{
    if (!key || !*key || !icon || !*icon) return;
    char k[160];
    lower_copy(k, sizeof(k), key);
    for (int i = 0; i < idx->desktop_count; i++) {
        if (strcmp(idx->desktops[i].key, k) == 0) {
            snprintf(idx->desktops[i].icon, sizeof(idx->desktops[i].icon), "%s", icon);
            return;
        }
    }
    if (idx->desktop_count == idx->desktop_cap) {
        int cap = idx->desktop_cap ? idx->desktop_cap * 2 : 256;
        idx->desktops = realloc(idx->desktops, (size_t)cap * sizeof(IconDesktop));
        idx->desktop_cap = cap;
    }
    IconDesktop *d = &idx->desktops[idx->desktop_count++];
    snprintf(d->key, sizeof(d->key), "%s", k);
    snprintf(d->icon, sizeof(d->icon), "%s", icon);
}

static void scan_theme_dir(IconIndex *idx, const char *theme, const char *size, int rank)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "/usr/share/icons/%s/%s/apps", theme, size);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".svg") != 0 && strcasecmp(dot, ".png") != 0 &&
            strcasecmp(dot, ".xpm") != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        char name[160];
        snprintf(name, sizeof(name), "%.*s", (int)(dot - e->d_name), e->d_name);
        int ext_rank = (strcasecmp(dot, ".svg") == 0) ? 0 :
                       (strcasecmp(dot, ".png") == 0) ? 1 : 2;
        add_icon(idx, name, path, rank * 10 + ext_rank);
    }
    closedir(d);
}

static void scan_theme(IconIndex *idx, const char *theme, int base_rank)
{
    static const char *sizes[] = {"scalable", "512x512", "256x256", "128x128",
                                  "96x96", "64x64", "48x48", "32x32", "24x24", "22x22"};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        scan_theme_dir(idx, theme, sizes[i], base_rank + (int)i);
}

static void scan_pixmaps(IconIndex *idx)
{
    DIR *d = opendir("/usr/share/pixmaps");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".png") != 0 && strcasecmp(dot, ".svg") != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%s", e->d_name);
        char name[160];
        snprintf(name, sizeof(name), "%.*s", (int)(dot - e->d_name), e->d_name);
        add_icon(idx, name, path, 900);
    }
    closedir(d);
}

static void scan_desktops(IconIndex *idx)
{
    int n = 0;
    char **items = list_dir("/usr/share/applications", &n);
    if (!items) return;
    for (int i = 0; i < n && idx->desktop_count < 8192; i++) {
        if (!has_suffix_ci(items[i], ".desktop")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/usr/share/applications/%s", items[i]);
        size_t len = 0;
        char *data = read_file(path, &len);
        if (!data) continue;
        char icon[160] = "";
        char exec[256] = "";
        char tryexec[256] = "";
        char *save = NULL;
        for (char *line = strtok_r(data, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            line = str_trim(line);
            if (strncmp(line, "Icon=", 5) == 0)
                snprintf(icon, sizeof(icon), "%s", str_trim(line + 5));
            else if (strncmp(line, "Exec=", 5) == 0)
                snprintf(exec, sizeof(exec), "%s", str_trim(line + 5));
            else if (strncmp(line, "TryExec=", 8) == 0)
                snprintf(tryexec, sizeof(tryexec), "%s", str_trim(line + 8));
        }
        free(data);
        if (!icon[0] || strchr(icon, '/')) continue;
        char base[160];
        snprintf(base, sizeof(base), "%s", items[i]);
        char *dot = strstr(base, ".desktop");
        if (dot) *dot = '\0';
        add_desktop(idx, base, icon);
        char *last = strrchr(base, '.');
        if (last && last[1]) add_desktop(idx, last + 1, icon);
        if (exec[0]) {
            char exe[160];
            snprintf(exe, sizeof(exe), "%s", exec);
            char *sp = strchr(exe, ' ');
            if (sp) *sp = '\0';
            const char *bn = strrchr(exe, '/');
            bn = bn ? bn + 1 : exe;
            if (strcmp(bn, "env") != 0 && strcmp(bn, "flatpak") != 0)
                add_desktop(idx, bn, icon);
        }
        if (tryexec[0]) {
            const char *bn = strrchr(tryexec, '/');
            bn = bn ? bn + 1 : tryexec;
            add_desktop(idx, bn, icon);
        }
    }
    free_strv(items, n);
}

static const char *find_icon_name(const IconIndex *idx, const char *name)
{
    if (!name || !*name) return NULL;
    char key[160];
    lower_copy(key, sizeof(key), name);
    int i = icon_find((IconIndex *)idx, key);
    return i >= 0 ? idx->icons[i].path : NULL;
}

static const char *find_desktop_icon(const IconIndex *idx, const char *key)
{
    char k[160];
    lower_copy(k, sizeof(k), key);
    for (int i = 0; i < idx->desktop_count; i++)
        if (strcmp(idx->desktops[i].key, k) == 0) return idx->desktops[i].icon;
    return NULL;
}

static const struct { const char *pkg; const char *icon; } ALIASES[] = {
    {"code", "vscode"}, {"code-oss", "vscode"},
    {"nautilus", "org.gnome.Nautilus"},
    {"ppsspp", "ppsspp-qt"}, {"dolphin-emu", "dolphin-emu"},
    {"htop", "utilities-terminal"}, {"top", "utilities-terminal"},
    {"7zip", "7zip"}, {"firefox", "firefox"}, {"chromium", "chromium"},
    {"steam", "steam"}, {"retroarch", "retroarch"}, {"vlc", "vlc"},
    {"mpv", "mpv"}, {"gimp", "gimp"}, {"krita", "krita"},
};

static void load_alias_file(IconIndex *idx)
{
    (void)idx;
    /* Aliases are intentionally compiled in for now; the file format is
     * documented in packaging/store/icon-aliases.tsv. */
}

void icon_index_init(IconIndex *idx)
{
    memset(idx, 0, sizeof(*idx));
}

void icon_index_free(IconIndex *idx)
{
    if (!idx) return;
    free(idx->icons);
    free(idx->slots);
    free(idx->desktops);
    memset(idx, 0, sizeof(*idx));
}

bool icon_index_build(IconIndex *idx)
{
    if (!idx) return false;
    static const struct { const char *theme; int rank; } themes[] = {
        {"Papirus", 0}, {"Papirus-Dark", 1}, {"Papirus-Light", 2},
        {"Tela", 3}, {"Tela-dark", 4}, {"Qogir", 5}, {"Qogir-dark", 6},
        {"hicolor", 10}, {"Adwaita", 11}, {"breeze", 12},
        {"breeze-dark", 13}, {"Yaru", 14}, {"elementary", 15},
    };
    for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++)
        scan_theme(idx, themes[i].theme, themes[i].rank * 20);
    scan_pixmaps(idx);
    scan_desktops(idx);
    load_alias_file(idx);
    return idx->icon_count > 0;
}

bool icon_index_lookup_name(const IconIndex *idx, const char *name,
                            char *out, size_t out_size)
{
    const char *p = find_icon_name(idx, name);
    if (!p) return false;
    snprintf(out, out_size, "%s", p);
    return true;
}

bool icon_index_resolve(const IconIndex *idx, const char *pkg,
                        char *out, size_t out_size)
{
    if (!idx || !pkg || !out || out_size == 0) return false;
    out[0] = '\0';

    const char *icon = NULL;
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (strcasecmp(pkg, ALIASES[i].pkg) == 0) {
            icon = ALIASES[i].icon;
            break;
        }
    }
    if (icon) {
        const char *p = find_icon_name(idx, icon);
        if (p) { snprintf(out, out_size, "%s", p); return true; }
    }
    const char *p = find_icon_name(idx, pkg);
    if (p) { snprintf(out, out_size, "%s", p); return true; }

    char base[160];
    snprintf(base, sizeof(base), "%s", pkg);
    char *suffix = strstr(base, "-bin");
    if (!suffix) suffix = strstr(base, "-git");
    if (!suffix) suffix = strstr(base, "-desktop");
    if (suffix) *suffix = '\0';
    if (strcmp(base, pkg) != 0) {
        p = find_icon_name(idx, base);
        if (p) { snprintf(out, out_size, "%s", p); return true; }
    }

    const char *desk = find_desktop_icon(idx, pkg);
    if (!desk) desk = find_desktop_icon(idx, base);
    if (desk) {
        p = find_icon_name(idx, desk);
        if (p) { snprintf(out, out_size, "%s", p); return true; }
    }

    /* 近似匹配：只在足够长的包名上做，并排除通用图标名。 */
    size_t plen = strlen(base);
    if (plen >= 5) {
        static const char *generic[] = {
            "wifi", "network-wireless", "play", "media-playback-start",
            "audio", "video", "utilities-terminal", "system-file-manager",
            "preferences-system", "applications-system", "folder", "file"
        };
        char key[160];
        lower_copy(key, sizeof(key), base);
        for (int i = 0; i < idx->icon_count; i++) {
            const char *name = idx->icons[i].name;
            if (strlen(name) < 5) continue;
            bool bad = false;
            for (size_t g = 0; g < sizeof(generic) / sizeof(generic[0]); g++)
                if (strcmp(name, generic[g]) == 0) bad = true;
            if (bad) continue;
            if (strstr(name, key) || strstr(key, name)) {
                snprintf(out, out_size, "%s", idx->icons[i].path);
                return true;
            }
        }
    }
    return false;
}
