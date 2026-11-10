// SPDX-License-Identifier: GPL-3.0-or-later
#include "libretro.h"
#include "catalog.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_CORES 64
#define MAX_DIRS 8
#define MAX_PATH 512

typedef struct {
    char name[64];
    char label[128];
    char path[MAX_PATH];
    char exts[1024];
} CoreEntry;

static CoreEntry g_cores[MAX_CORES];
static int g_core_count = -1;

static void trim_ws(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[--n] = '\0';
    }
}

static void unquote(char *s)
{
    trim_ws(s);
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void prettify_name(const char *name, char *out, size_t out_size)
{
    size_t i = 0;
    bool upper = true;
    for (; name[i] && i + 1 < out_size; i++) {
        char c = name[i];
        if (c == '_' || c == '-') {
            out[i] = ' ';
            upper = true;
        } else if (upper) {
            out[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            upper = false;
        } else {
            out[i] = c;
        }
    }
    out[i] = '\0';
}

static int core_dirs(char dirs[][MAX_PATH], int max)
{
    int n = 0;
    const char *home = getenv("HOME");
    if (home && *home && n < max)
        snprintf(dirs[n++], MAX_PATH, "%s/.config/retroarch/cores", home);
    static const char *sys[] = {
        "/usr/lib/libretro",
        "/usr/lib/aarch64-linux-gnu/libretro",
        "/usr/lib/x86_64-linux-gnu/libretro",
        "/usr/lib64/libretro",
        NULL,
    };
    for (int i = 0; sys[i] && n < max; i++)
        snprintf(dirs[n++], MAX_PATH, "%s", sys[i]);
    return n;
}

static bool parse_info(const char *path, char *label, size_t label_size,
                       char *exts, size_t exts_size)
{
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return false;
    (void)len;

    char corename[128] = "";
    char display[192] = "";
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_ws(key);
        unquote(val);
        if (strcmp(key, "corename") == 0 && *val)
            snprintf(corename, sizeof(corename), "%s", val);
        else if (strcmp(key, "display_name") == 0 && *val)
            snprintf(display, sizeof(display), "%s", val);
        else if (strcmp(key, "supported_extensions") == 0 && *val)
            snprintf(exts, exts_size, "%s", val);
    }
    free(data);

    if (corename[0]) snprintf(label, label_size, "%s", corename);
    else if (display[0]) snprintf(label, label_size, "%s", display);
    else label[0] = '\0';
    return true;
}

static bool build_path(char *out, size_t out_size, const char *dir, const char *file)
{
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    if (dl + 1 + fl >= out_size) return false;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, file, fl + 1);
    return true;
}

static bool retroarch_available(void)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    char *copy = xstrdup(path);
    char full[1024];
    char *save = NULL;
    bool found = false;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        if (build_path(full, sizeof(full), dir, "retroarch") &&
            access(full, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

static void load_info(const char *name, const char *core_dir,
                      char *label, size_t label_size,
                      char *exts, size_t exts_size)
{
    label[0] = '\0';
    exts[0] = '\0';
    char file[96];
    snprintf(file, sizeof(file), "%.63s_libretro.info", name);
    char path[MAX_PATH];
    if (build_path(path, sizeof(path), "/usr/share/libretro/info", file) &&
        parse_info(path, label, label_size, exts, exts_size))
        return;

    if (build_path(path, sizeof(path), core_dir, file) &&
        parse_info(path, label, label_size, exts, exts_size))
        return;

    const char *home = getenv("HOME");
    if (home && *home) {
        char home_dir[MAX_PATH];
        if (build_path(home_dir, sizeof(home_dir), home, ".config/retroarch/cores") &&
            build_path(path, sizeof(path), home_dir, file))
            parse_info(path, label, label_size, exts, exts_size);
    }
}

static bool core_seen(const char *name)
{
    for (int i = 0; i < g_core_count; i++)
        if (strcmp(g_cores[i].name, name) == 0) return true;
    return false;
}

static void scan_core_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    static const char suffix[] = "_libretro.so";
    const size_t suffix_len = sizeof(suffix) - 1;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= suffix_len ||
            strcmp(ent->d_name + len - suffix_len, suffix) != 0)
            continue;
        char name[64];
        size_t name_len = len - suffix_len;
        if (name_len >= sizeof(name)) continue;
        memcpy(name, ent->d_name, name_len);
        name[name_len] = '\0';
        if (core_seen(name)) continue;
        if (g_core_count >= MAX_CORES) break;

        CoreEntry *e = &g_cores[g_core_count];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);
        if (!build_path(e->path, sizeof(e->path), dir, ent->d_name)) continue;
        load_info(name, dir, e->label, sizeof(e->label), e->exts, sizeof(e->exts));
        if (!e->label[0]) prettify_name(name, e->label, sizeof(e->label));
        g_core_count++;
    }
    closedir(dp);
}

static int cmp_entry_label(const void *pa, const void *pb)
{
    const CoreEntry *a = pa;
    const CoreEntry *b = pb;
    return strcasecmp(a->label, b->label);
}

static void ensure_inventory(void)
{
    if (g_core_count >= 0) return;
    g_core_count = 0;
    char dirs[MAX_DIRS][MAX_PATH];
    int n = core_dirs(dirs, MAX_DIRS);
    for (int i = 0; i < n; i++) scan_core_dir(dirs[i]);
    qsort(g_cores, (size_t)g_core_count, sizeof(g_cores[0]), cmp_entry_label);
}

void libretro_refresh(void)
{
    g_core_count = -1;
}

int libretro_scan(LibretroCore *out, int max)
{
    if (!retroarch_available()) return 0;
    ensure_inventory();
    int n = g_core_count < max ? g_core_count : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].name, g_cores[i].name, sizeof(out[i].name));
        memcpy(out[i].label, g_cores[i].label, sizeof(out[i].label));
        memcpy(out[i].path, g_cores[i].path, sizeof(out[i].path));
        out[i].name[sizeof(out[i].name) - 1] = '\0';
        out[i].label[sizeof(out[i].label) - 1] = '\0';
        out[i].path[sizeof(out[i].path) - 1] = '\0';
    }
    return n;
}

static bool ext_is_generic(const char *ext)
{
    static const char *generic[] = {
        "zip", "7z", "rar", "gz", "iso", "bin", "cue", "chd", "gdi",
        "cso", "rvz", "wbfs", "wad", "rom", "dat", "m3u", "ccd", "img",
        "mdf", "toc", "nrg", "pbp", "ecm", NULL,
    };
    for (int i = 0; generic[i]; i++)
        if (str_ieq(ext, generic[i])) return true;
    return false;
}

static int split_list(const char *src, char sep, char out[][32], int max)
{
    int n = 0;
    if (!src) return 0;
    while (*src && n < max) {
        while (*src == ' ' || *src == '\t' || *src == sep) src++;
        if (!*src) break;
        int len = 0;
        while (*src && *src != sep && *src != ' ' && *src != '\t') {
            if (len < 31) out[n][len++] = *src;
            src++;
        }
        out[n][len] = '\0';
        if (len > 0) n++;
        while (*src && *src != sep) src++;
    }
    return n;
}

static bool platform_ext_matches(const PlatformDef *def, const char *core_exts)
{
    if (!def || !def->extensions || !core_exts[0]) return false;
    char plat[32][32];
    char core[64][32];
    int np = split_list(def->extensions, ',', plat, 32);
    int nc = split_list(core_exts, '|', core, 64);
    bool any_specific = false;
    for (int i = 0; i < np; i++) {
        if (ext_is_generic(plat[i])) continue;
        any_specific = true;
        for (int j = 0; j < nc; j++)
            if (str_ieq(plat[i], core[j])) return true;
    }
    (void)any_specific;
    return false;
}

static bool core_is_candidate(const char *name, const char *const *cand, int count)
{
    for (int i = 0; i < count; i++)
        if (strcmp(name, cand[i]) == 0) return true;
    return false;
}

static int cmp_core(const void *pa, const void *pb)
{
    const LibretroCore *a = pa;
    const LibretroCore *b = pb;
    if (a->recommended != b->recommended)
        return a->recommended ? -1 : 1;
    return strcasecmp(a->label, b->label);
}

int libretro_compatible(const char *platform_id, LibretroCore *out, int max)
{
    if (!retroarch_available()) return 0;
    ensure_inventory();
    if (!platform_id || !*platform_id || max <= 0) return 0;

    const char *cand[16];
    int nc = catalog_core_candidates(platform_id, cand, 16);
    const PlatformDef *def = catalog_by_id(platform_id);

    int n = 0;
    for (int i = 0; i < g_core_count && n < max; i++) {
        CoreEntry *e = &g_cores[i];
        bool comp = core_is_candidate(e->name, cand, nc);
        if (!comp) comp = platform_ext_matches(def, e->exts);
        if (!comp) continue;
        LibretroCore *o = &out[n++];
        memset(o, 0, sizeof(*o));
        snprintf(o->name, sizeof(o->name), "%s", e->name);
        snprintf(o->label, sizeof(o->label), "%s", e->label);
        snprintf(o->path, sizeof(o->path), "%s", e->path);
        o->compatible = true;
    }

    int rec = -1;
    for (int i = 0; i < nc && rec < 0; i++) {
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].name, cand[i]) == 0) { rec = j; break; }
        }
    }
    if (rec < 0 && n > 0) rec = 0;
    if (rec >= 0) out[rec].recommended = true;

    qsort(out, (size_t)n, sizeof(out[0]), cmp_core);
    return n;
}

static bool copy_path_token(const char *p, char *out, size_t out_size)
{
    while (*p == ' ' || *p == '\t') p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') quote = *p++;
    size_t i = 0;
    while (*p && (quote ? *p != quote : (*p != ' ' && *p != '\t'))) {
        if (i + 1 < out_size) out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

bool libretro_command_core(const char *command, char *path, size_t path_size)
{
    if (!command || !*command || !path || path_size == 0) return false;
    path[0] = '\0';
    const char *p = command;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if ((len == 2 && strncmp(start, "-L", 2) == 0) ||
            (len == 10 && strncmp(start, "--libretro", 10) == 0))
            return copy_path_token(p, path, path_size);
        if (len > 11 && strncmp(start, "--libretro=", 11) == 0)
            return copy_path_token(start + 11, path, path_size);
    }
    return false;
}

const char *libretro_label_for_name(const char *name, char *buf, size_t buf_size)
{
    ensure_inventory();
    if (name && *name) {
        for (int i = 0; i < g_core_count; i++) {
            if (strcmp(g_cores[i].name, name) == 0) {
                snprintf(buf, buf_size, "%s", g_cores[i].label);
                return buf;
            }
        }
        prettify_name(name, buf, buf_size);
    } else {
        snprintf(buf, buf_size, "%s", "");
    }
    return buf;
}
