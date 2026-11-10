// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "library.h"
#include "catalog.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Metadata and media index helpers ─────────────────────────────────────── */

typedef struct {
    char *key_path;      /* absolute ROM path if known */
    char *key_name;      /* normalized game name */
    char *name;
    char *description;
    char *developer;
    char *publisher;
    char *genre;
    char *release;
    char *box;
    char *logo;
    int players;
} MetaEntry;

typedef struct {
    MetaEntry *items;
    int count;
    int cap;
    char *base_dir;
} MetaList;

typedef struct {
    char *norm;
    char *path;
    int score;
} MediaEntry;

typedef struct {
    MediaEntry *items;
    int count;
    int cap;
} MediaIndex;

static void meta_free(MetaList *m)
{
    for (int i = 0; i < m->count; i++) {
        free(m->items[i].key_path);
        free(m->items[i].key_name);
        free(m->items[i].name);
        free(m->items[i].description);
        free(m->items[i].developer);
        free(m->items[i].publisher);
        free(m->items[i].genre);
        free(m->items[i].release);
        free(m->items[i].box);
        free(m->items[i].logo);
    }
    free(m->items);
    free(m->base_dir);
    memset(m, 0, sizeof(*m));
}

static MetaEntry *meta_add(MetaList *m)
{
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->items = xrealloc(m->items, (size_t)m->cap * sizeof(MetaEntry));
    }
    MetaEntry *e = &m->items[m->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void media_add(MediaIndex *idx, const char *path, const char *norm, int score)
{
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->items[i].norm, norm) == 0) {
            if (score > idx->items[i].score) {
                free(idx->items[i].path);
                idx->items[i].path = xstrdup(path);
                idx->items[i].score = score;
            }
            return;
        }
    }
    if (idx->count == idx->cap) {
        idx->cap = idx->cap ? idx->cap * 2 : 128;
        idx->items = xrealloc(idx->items, (size_t)idx->cap * sizeof(MediaEntry));
    }
    idx->items[idx->count].norm = xstrdup(norm);
    idx->items[idx->count].path = xstrdup(path);
    idx->items[idx->count].score = score;
    idx->count++;
}

static const char *media_lookup(const MediaIndex *idx, const char *norm)
{
    const char *best = NULL;
    int best_score = -1;
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->items[i].norm, norm) == 0 && idx->items[i].score > best_score) {
            best = idx->items[i].path;
            best_score = idx->items[i].score;
        }
    }
    return best;
}

static void media_free(MediaIndex *idx)
{
    for (int i = 0; i < idx->count; i++) {
        free(idx->items[i].norm);
        free(idx->items[i].path);
    }
    free(idx->items);
    memset(idx, 0, sizeof(*idx));
}

static int image_score_for_dir(const char *dir)
{
    char *lower = xstrdup(dir);
    str_lower(lower);
    int score = 30;
    if (strstr(lower, "box2dfront") || strstr(lower, "boxfront") || strstr(lower, "box_art") ||
        strstr(lower, "boxart") || strstr(lower, "covers") || strstr(lower, "cover") ||
        strstr(lower, "front") || strstr(lower, "artwork")) score = 90;
    else if (strstr(lower, "title") || strstr(lower, "screenshot") || strstr(lower, "snap")) score = 25;
    else if (strstr(lower, "logo") || strstr(lower, "marquee") || strstr(lower, "wheel")) score = 55;
    else if (strstr(lower, "back")) score = 35;
    free(lower);
    return score;
}

/* ── XML / Pegasus metadata ────────────────────────────────────────────────── */

static bool xml_tag(const char *block, const char *tag, char **out)
{
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *p = strstr(block, open);
    if (!p) return false;
    p += strlen(open);
    if (*p == '>') p++;
    else if ((p = strchr(p, '>'))) p++;
    else return false;
    const char *e = strstr(p, close);
    if (!e) return false;
    size_t n = (size_t)(e - p);
    char *raw = xmalloc(n + 1);
    memcpy(raw, p, n);
    raw[n] = '\0';
    char *decoded = html_decode(raw);
    free(raw);
    *out = str_trim(decoded);
    return true;
}

static char *resolve_relative(const char *base_dir, const char *path)
{
    if (!path || !*path) return NULL;
    char *clean = xstrdup(path);
    char *t = str_trim(clean);
    char *decoded = html_decode(t);
    free(clean);
    char *result;
    if (decoded[0] == '/') result = xstrdup(decoded);
    else if (decoded[0] == '~') result = expand_home(decoded);
    else result = path_join(base_dir, decoded);
    free(decoded);
    return result;
}

static void parse_gamelist(const char *file, const char *base_dir, MetaList *out)
{
    size_t len = 0;
    char *data = read_file(file, &len);
    if (!data) return;
    const char *p = data;
    while ((p = strstr(p, "<game"))) {
        const char *end = strstr(p, "</game>");
        if (!end) break;
        size_t n = (size_t)(end - p) + 7;
        char *block = xmalloc(n + 1);
        memcpy(block, p, n);
        block[n] = '\0';
        char *path = NULL, *name = NULL;
        if (xml_tag(block, "path", &path) || xml_tag(block, "name", &name)) {
            MetaEntry *e = meta_add(out);
            e->key_path = path ? resolve_relative(base_dir, path) : NULL;
            e->key_name = normalize_title(name ? name : (path ? path_basename(path) : ""));
            e->name = name && *name ? xstrdup(name) : NULL;
            char *tmp = NULL;
            if (xml_tag(block, "desc", &tmp)) { e->description = xstrdup(tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "developer", &tmp)) { e->developer = xstrdup(tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "publisher", &tmp)) { e->publisher = xstrdup(tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "genre", &tmp)) { e->genre = xstrdup(tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "releasedate", &tmp) || xml_tag(block, "release", &tmp)) {
                e->release = xstrdup(tmp); free(tmp); tmp = NULL;
            }
            if (xml_tag(block, "players", &tmp)) { e->players = atoi(tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "image", &tmp)) { e->box = resolve_relative(base_dir, tmp); free(tmp); tmp = NULL; }
            if (xml_tag(block, "thumbnail", &tmp)) {
                if (!e->box) e->box = resolve_relative(base_dir, tmp);
                free(tmp); tmp = NULL;
            }
            if (xml_tag(block, "marquee", &tmp)) { e->logo = resolve_relative(base_dir, tmp); free(tmp); tmp = NULL; }
            free(path);
            free(name);
        }
        free(block);
        p = end + 7;
    }
    free(data);
}

static char **split_lines(char *data, int *count)
{
    char **lines = NULL;
    int n = 0, cap = 0;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (n == cap) { cap = cap ? cap * 2 : 64; lines = xrealloc(lines, (size_t)cap * sizeof(char *)); }
        lines[n++] = line;
    }
    *count = n;
    return lines;
}

static void parse_pegasus(const char *file, const char *base_dir, MetaList *out)
{
    size_t len = 0;
    char *data = read_file(file, &len);
    if (!data) return;
    int nlines = 0;
    char **lines = split_lines(data, &nlines);
    MetaEntry *cur = NULL;
    for (int i = 0; i < nlines; i++) {
        char *line = str_trim(lines[i]);
        if (!*line || *line == '#') continue;
        if (strncasecmp(line, "game:", 5) == 0) {
            cur = meta_add(out);
            char *v = str_trim(line + 5);
            if (*v) { cur->name = xstrdup(v); cur->key_name = normalize_title(v); }
            continue;
        }
        if (!cur) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = str_trim(line);
        char *val = str_trim(colon + 1);
        if (str_ieq(key, "file")) {
            free(cur->key_path);
            cur->key_path = resolve_relative(base_dir, val);
            if (!cur->key_name) cur->key_name = normalize_title(val);
        } else if (str_ieq(key, "name")) {
            free(cur->name);
            cur->name = xstrdup(val);
            free(cur->key_name);
            cur->key_name = normalize_title(val);
        } else if (str_ieq(key, "description") || str_ieq(key, "summary")) {
            free(cur->description);
            char *decoded = html_decode(val);
            cur->description = xstrdup(decoded);
            free(decoded);
        } else if (str_ieq(key, "developer")) {
            free(cur->developer); cur->developer = xstrdup(val);
        } else if (str_ieq(key, "publisher")) {
            free(cur->publisher); cur->publisher = xstrdup(val);
        } else if (str_ieq(key, "genre")) {
            free(cur->genre); cur->genre = xstrdup(val);
        } else if (str_ieq(key, "release") || str_ieq(key, "releasedate")) {
            free(cur->release); cur->release = xstrdup(val);
        } else if (str_ieq(key, "players")) {
            cur->players = atoi(val);
        } else if (str_ieq(key, "assets.boxfront") || str_ieq(key, "assets.box2dfront") ||
                   str_ieq(key, "assets.cover") || str_ieq(key, "assets.screenshot")) {
            free(cur->box);
            cur->box = resolve_relative(base_dir, val);
        } else if (str_ieq(key, "assets.logo") || str_ieq(key, "assets.wheel") ||
                   str_ieq(key, "assets.marquee")) {
            free(cur->logo);
            cur->logo = resolve_relative(base_dir, val);
        }
    }
    free(lines);
    free(data);
    (void)len;
}

static MetaEntry *meta_find(MetaList *m, const char *rom_path, const char *name)
{
    char *norm = normalize_title(name);
    char *abs = path_absolute(rom_path);
    MetaEntry *found = NULL;
    for (int i = 0; i < m->count; i++) {
        MetaEntry *e = &m->items[i];
        if (e->key_path && abs && strcmp(e->key_path, abs) == 0) { found = e; break; }
    }
    if (!found) {
        for (int i = 0; i < m->count; i++) {
            MetaEntry *e = &m->items[i];
            if (!e->key_name) continue;
            if (strcmp(e->key_name, norm) == 0) { found = e; break; }
        }
    }
    if (!found && abs) {
        const char *base = path_basename(abs);
        for (int i = 0; i < m->count; i++) {
            MetaEntry *e = &m->items[i];
            if (e->key_path && strcmp(path_basename(e->key_path), base) == 0) { found = e; break; }
        }
    }
    free(norm);
    free(abs);
    return found;
}

/* ── Library containers ───────────────────────────────────────────────────── */

void library_init(Library *lib)
{
    memset(lib, 0, sizeof(*lib));
}

static void game_free(Game *g)
{
    free(g->id); free(g->platform_id); free(g->name); free(g->sort_name);
    free(g->rom_path); free(g->cover_path); free(g->logo_path);
    free(g->description); free(g->developer); free(g->publisher);
    free(g->genre); free(g->release_date); free(g->command);
}

void library_free(Library *lib)
{
    for (int i = 0; i < lib->platform_count; i++) {
        free(lib->platforms[i].id);
        free(lib->platforms[i].name);
        free(lib->platforms[i].short_name);
        free(lib->platforms[i].logo);
        free(lib->platforms[i].root_dir);
        free(lib->platforms[i].cover_path);
    }
    free(lib->platforms);
    for (int i = 0; i < lib->game_count; i++) game_free(&lib->games[i]);
    free(lib->games);
    memset(lib, 0, sizeof(*lib));
}

Game *library_find_game(Library *lib, const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < lib->game_count; i++) {
        if (strcmp(lib->games[i].id, id) == 0) return &lib->games[i];
    }
    return NULL;
}

Platform *library_find_platform(Library *lib, const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < lib->platform_count; i++) {
        if (str_ieq(lib->platforms[i].id, id)) return &lib->platforms[i];
    }
    return NULL;
}

static Platform *platform_get(Library *lib, const char *id, const char *name,
                              const char *short_name, const char *logo,
                              const char *root_dir)
{
    Platform *p = library_find_platform(lib, id);
    if (p) {
        if (!p->root_dir && root_dir) p->root_dir = xstrdup(root_dir);
        return p;
    }
    lib->platforms = xrealloc(lib->platforms, (size_t)(lib->platform_count + 1) * sizeof(Platform));
    p = &lib->platforms[lib->platform_count++];
    memset(p, 0, sizeof(*p));
    p->id = xstrdup(id);
    p->name = xstrdup(name ? name : id);
    p->short_name = xstrdup(short_name ? short_name : p->name);
    p->logo = logo ? xstrdup(logo) : catalog_logo_path(id);
    p->root_dir = root_dir ? xstrdup(root_dir) : NULL;
    return p;
}

static Game *game_add(Library *lib)
{
    lib->games = xrealloc(lib->games, (size_t)(lib->game_count + 1) * sizeof(Game));
    Game *g = &lib->games[lib->game_count++];
    memset(g, 0, sizeof(*g));
    return g;
}

static void slugify(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (isalnum(c)) *w++ = (char)tolower(c);
        else if (w > s && w[-1] != '_') *w++ = '_';
    }
    while (w > s && w[-1] == '_') w--;
    *w = '\0';
}

/* ── Scan ─────────────────────────────────────────────────────────────────── */

typedef struct {
    Library *lib;
    const PlatformDef *def;
    Platform *platform;
    MetaList meta;
    MediaIndex media;
    ScanProgressCb progress;
    void *progress_user;
    int file_count;
    int last_report;
    int added;
} ScanCtx;

static bool is_media_dir(const char *name)
{
    return str_ieq(name, "media") || str_ieq(name, "images") || str_ieq(name, "image") ||
           str_ieq(name, "boxart") || str_ieq(name, "covers") || str_ieq(name, "cover") ||
           str_ieq(name, "screenshots") || str_ieq(name, "screenshot") || str_ieq(name, "snaps") ||
           str_ieq(name, "videos") || str_ieq(name, "video") || str_ieq(name, "manuals") ||
           str_ieq(name, "bios") || str_ieq(name, "saves") || str_ieq(name, "save") ||
           str_ieq(name, "artwork") || str_ieq(name, "downloads") || str_ieq(name, "metadata");
}

static void index_images(ScanCtx *ctx, const char *dir, int depth);

static void scan_dir(ScanCtx *ctx, const char *dir, int depth)
{
    if (depth > 8) return;
    int n = 0;
    char **entries = list_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        char *path = path_join(dir, entries[i]);
        struct stat st;
        if (lstat(path, &st) != 0) { free(path); continue; }
        if (S_ISLNK(st.st_mode)) { /* follow files but not directories */ }
        if (S_ISDIR(st.st_mode)) {
            const char *name = entries[i];
            if (is_media_dir(name)) index_images(ctx, path, depth + 1);
            else scan_dir(ctx, path, depth + 1);
            free(path);
            continue;
        }
        if (!S_ISREG(st.st_mode)) { free(path); continue; }
        if (catalog_is_image(path)) {
            char *norm = normalize_title(path_basename(path));
            if (*norm) media_add(&ctx->media, path, norm, image_score_for_dir(dir));
            free(norm);
            free(path);
            continue;
        }
        const char *ext = path_extension(path);
        bool accepted = ctx->def ? catalog_accepts_extension(ctx->def, ext) : catalog_is_rom(path);
        if (!accepted || !ext[0]) { free(path); continue; }
        /* Ignore obvious non-ROM sidecar files. */
        if (str_ieq(ext, "txt") || str_ieq(ext, "xml") || str_ieq(ext, "json") ||
            str_ieq(ext, "dat") || str_ieq(ext, "nfo")) { free(path); continue; }

        char *name = pretty_rom_name(entries[i]);
        MetaEntry *meta = meta_find(&ctx->meta, path, name);
        char *abs = path_absolute(path);
        bool duplicate = false;
        for (int gi = 0; gi < ctx->lib->game_count; gi++) {
            if (strcmp(ctx->lib->games[gi].rom_path, abs) == 0) { duplicate = true; break; }
        }
        if (duplicate) { free(abs); free(name); free(path); continue; }
        Game *g = game_add(ctx->lib);
        g->id = xasprintf("%08x%08x", fnv1a(abs), (uint32_t)st.st_size);
        g->platform_id = xstrdup(ctx->platform->id);
        g->name = xstrdup(meta && meta->name ? meta->name : name);
        g->sort_name = normalize_title(g->name);
        g->rom_path = abs;
        g->description = xstrdup(meta && meta->description ? meta->description : "");
        g->developer = xstrdup(meta && meta->developer ? meta->developer : "");
        g->publisher = xstrdup(meta && meta->publisher ? meta->publisher : "");
        g->genre = xstrdup(meta && meta->genre ? meta->genre : "");
        g->release_date = xstrdup(meta && meta->release ? meta->release : "");
        g->players = meta ? meta->players : 0;
        g->favorite = false;
        g->last_played = 0;
        g->play_count = 0;
        g->play_seconds = 0;
        g->command = xstrdup("");

        /* Cover: metadata asset first, then a sibling image, then the media index. */
        if (meta && meta->box && file_exists(meta->box)) {
            g->cover_path = xstrdup(meta->box);
        } else {
            char *norm_file = normalize_title(path_basename(path));
            const char *hit = media_lookup(&ctx->media, norm_file);
            if (hit) g->cover_path = xstrdup(hit);
            if (!hit) {
                char *norm_name = normalize_title(g->name);
                const char *hit2 = media_lookup(&ctx->media, norm_name);
                if (hit2) g->cover_path = xstrdup(hit2);
                free(norm_name);
            }
            free(norm_file);
        }
        if (!g->cover_path) {
            const char *cand[] = {".png", ".jpg", ".jpeg", ".webp"};
            for (int ci = 0; ci < 4; ci++) {
                char *stem = xstrdup(path);
                char *dot = strrchr(stem, '.');
                if (dot) *dot = '\0';
                char *test = xasprintf("%s%s", stem, cand[ci]);
                if (file_exists(test)) { g->cover_path = test; free(stem); break; }
                free(test);
                free(stem);
            }
        }
        if (meta && meta->logo && file_exists(meta->logo)) g->logo_path = xstrdup(meta->logo);
        if (!ctx->platform->cover_path && g->cover_path) ctx->platform->cover_path = xstrdup(g->cover_path);
        ctx->platform->game_count++;
        ctx->added++;
        free(name);
        free(path);
    }
    free_strv(entries, n);
    ctx->file_count += n;
    if (ctx->progress && ctx->file_count - ctx->last_report > 150) {
        ctx->last_report = ctx->file_count;
        char msg[512];
        snprintf(msg, sizeof(msg), "正在扫描 %s / %d 个游戏", ctx->platform->name, ctx->added);
        ctx->progress(ctx->progress_user, msg, -1);
    }
}

static void index_images(ScanCtx *ctx, const char *dir, int depth)
{
    if (depth > 10) return;
    int n = 0;
    char **entries = list_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        char *path = path_join(dir, entries[i]);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) index_images(ctx, path, depth + 1);
            else if (S_ISREG(st.st_mode) && catalog_is_image(path)) {
                char *norm = normalize_title(path_basename(path));
                if (*norm) media_add(&ctx->media, path, norm, image_score_for_dir(dir));
                free(norm);
            }
        }
        free(path);
    }
    free_strv(entries, n);
}

static void scan_platform_dir(Library *lib, const char *dir, const PlatformDef *def,
                              const char *display_name, const char *platform_id,
                              ScanProgressCb cb, void *ud)
{
    ScanCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.lib = lib;
    ctx.def = def;
    ctx.progress = cb;
    ctx.progress_user = ud;
    ctx.platform = platform_get(lib, platform_id,
                                display_name ? display_name : (def ? def->name : path_basename(dir)),
                                def ? def->short_name : NULL,
                                def ? NULL : NULL, dir);
    if (def) {
        free(ctx.platform->logo);
        ctx.platform->logo = catalog_logo_path(def->id);
    }

    char *gamelist = path_join(dir, "gamelist.xml");
    char *pegasus = path_join(dir, "metadata.pegasus.txt");
    if (file_exists(gamelist)) parse_gamelist(gamelist, dir, &ctx.meta);
    if (file_exists(pegasus)) parse_pegasus(pegasus, dir, &ctx.meta);
    free(gamelist);
    free(pegasus);

    /* Index artwork first so cover matching does not depend on directory order. */
    index_images(&ctx, dir, 0);
    scan_dir(&ctx, dir, 0);
    media_free(&ctx.media);
    meta_free(&ctx.meta);
    if (cb) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s / %d 个游戏", ctx.platform->name, ctx.platform->game_count);
        cb(ud, msg, -1);
    }
}

static const PlatformDef *infer_platform_dir(const char *dir, char **out_id)
{
    (void)out_id;
    int n = 0;
    char **entries = list_dir(dir, &n);
    int best = -1;
    const PlatformDef *best_def = NULL;
    for (int i = 0; i < n; i++) {
        char *path = path_join(dir, entries[i]);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            int n2 = 0;
            char **sub = list_dir(path, &n2);
            int counts[256] = {0};
            for (int j = 0; j < n2; j++) {
                const char *ext = path_extension(sub[j]);
                const PlatformDef *def = catalog_by_extension(ext);
                if (def) {
                    int idx = (int)(def - catalog_all(NULL));
                    if (idx >= 0 && idx < 256) counts[idx]++;
                }
            }
            for (int j = 0; j < 256; j++) {
                if (counts[j] > best) {
                    const PlatformDef *all = catalog_all(NULL);
                    best = counts[j];
                    best_def = &all[j];
                }
            }
            free_strv(sub, n2);
        } else {
            const PlatformDef *def = catalog_by_extension(path_extension(entries[i]));
            if (def) {
                int idx = (int)(def - catalog_all(NULL));
                if (idx >= 0 && idx < 256) {
                    int c = 1;
                    if (c > best) { best = c; best_def = def; }
                }
            }
        }
        free(path);
    }
    free_strv(entries, n);
    return best_def;
}

bool library_scan(Library *lib, const Config *cfg, ScanProgressCb cb, void *ud)
{
    Library fresh;
    library_init(&fresh);
    int total_roots = cfg->root_count;
    for (int r = 0; r < total_roots; r++) {
        const char *root = cfg->roots[r];
        if (!dir_exists(root)) continue;
        if (cb) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "正在扫描 %s", root);
            cb(ud, msg, total_roots > 0 ? (r * 100 / total_roots) : 0);
        }
        const PlatformDef *root_def = catalog_by_folder(path_basename(root));
        if (root_def) {
            scan_platform_dir(&fresh, root, root_def, NULL, root_def->id, cb, ud);
            continue;
        }

        int n = 0;
        char **entries = list_dir(root, &n);
        bool found_subdir = false;
        for (int i = 0; i < n; i++) {
            char *path = path_join(root, entries[i]);
            struct stat st;
            if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                const PlatformDef *def = catalog_by_folder(entries[i]);
                if (def) {
                    scan_platform_dir(&fresh, path, def, NULL, def->id, cb, ud);
                    found_subdir = true;
                } else {
                    /* A user folder with a non-standard name: infer from its contents. */
                    const PlatformDef *inferred = infer_platform_dir(path, NULL);
                    if (inferred) {
                        char *id = xstrdup(inferred->id);
                        scan_platform_dir(&fresh, path, inferred, entries[i], id, cb, ud);
                        free(id);
                        found_subdir = true;
                    } else {
                        /* Unknown system: still expose its files under a custom platform. */
                        int n2 = 0;
                        char **sub = list_dir(path, &n2);
                        bool has_rom = false;
                        for (int j = 0; j < n2; j++) {
                            if (catalog_is_rom(sub[j])) { has_rom = true; break; }
                        }
                        free_strv(sub, n2);
                        if (has_rom) {
                            char slug[128];
                            snprintf(slug, sizeof(slug), "%s", entries[i]);
                            slugify(slug);
                            char id[160];
                            snprintf(id, sizeof(id), "custom_%s", slug[0] ? slug : "games");
                            scan_platform_dir(&fresh, path, NULL, entries[i], id, cb, ud);
                            found_subdir = true;
                        }
                    }
                }
            }
            free(path);
        }
        if (!found_subdir) {
            /* Root contains loose ROM files: group them by detected extension platform. */
            for (int i = 0; i < n; i++) {
                char *path = path_join(root, entries[i]);
                const char *ext = path_extension(entries[i]);
                const PlatformDef *def = catalog_by_extension(ext);
                if (def && file_exists(path)) {
                    /* Scan the root once per platform would duplicate; instead make one
                     * platform entry and add loose files directly. */
                    (void)path;
                }
                free(path);
            }
            /* Use a single inferred platform for the whole root. */
            const PlatformDef *inferred = infer_platform_dir(root, NULL);
            if (inferred) scan_platform_dir(&fresh, root, inferred, NULL, inferred->id, cb, ud);
        }
        free_strv(entries, n);
    }

    /* Refresh counts and return a stable platform order: all platforms alphabetically. */
    for (int i = 0; i < fresh.platform_count; i++) {
        int count = 0;
        for (int j = 0; j < fresh.game_count; j++) {
            if (strcmp(fresh.games[j].platform_id, fresh.platforms[i].id) == 0) count++;
        }
        fresh.platforms[i].game_count = count;
    }

    /* Preserve favourites, playtime and per-game launch overrides across scans. */
    for (int i = 0; i < fresh.game_count; i++) {
        for (int j = 0; j < lib->game_count; j++) {
            if (strcmp(fresh.games[i].rom_path, lib->games[j].rom_path) == 0) {
                fresh.games[i].favorite = lib->games[j].favorite;
                fresh.games[i].last_played = lib->games[j].last_played;
                fresh.games[i].play_count = lib->games[j].play_count;
                fresh.games[i].play_seconds = lib->games[j].play_seconds;
                free(fresh.games[i].command);
                fresh.games[i].command = xstrdup(lib->games[j].command ? lib->games[j].command : "");
                break;
            }
        }
    }
    library_free(lib);
    *lib = fresh;
    library_save_cache(lib);
    if (cb) cb(ud, "扫描完成", 100);
    return true;
}

/* ── Query / sorting ──────────────────────────────────────────────────────── */

static SortMode g_sort_mode;
static FilterMode g_filter_mode;

static int cmp_sort(const void *pa, const void *pb)
{
    const Game *a = *(const Game *const *)pa;
    const Game *b = *(const Game *const *)pb;
    if (g_filter_mode == FILTER_ALL && g_sort_mode == SORT_NAME) {
        return strcoll(a->sort_name ? a->sort_name : a->name, b->sort_name ? b->sort_name : b->name);
    }
    if (g_sort_mode == SORT_RECENT) {
        if (a->last_played != b->last_played) return a->last_played < b->last_played ? 1 : -1;
    } else if (g_sort_mode == SORT_MOST_PLAYED) {
        if (a->play_count != b->play_count) return a->play_count < b->play_count ? 1 : -1;
        if (a->play_seconds != b->play_seconds) return a->play_seconds < b->play_seconds ? 1 : -1;
    } else if (g_sort_mode == SORT_FAVORITES_FIRST) {
        if (a->favorite != b->favorite) return a->favorite ? -1 : 1;
    }
    return strcoll(a->sort_name ? a->sort_name : a->name, b->sort_name ? b->sort_name : b->name);
}

GameList library_query(Library *lib, const char *platform_id, SortMode sort,
                       FilterMode filter, const char *query)
{
    GameList list = {0};
    if (!platform_id) platform_id = "all";
    bool all = str_ieq(platform_id, "all") || str_ieq(platform_id, "recent") ||
               str_ieq(platform_id, "favorites");
    for (int i = 0; i < lib->game_count; i++) {
        Game *g = &lib->games[i];
        if (g->hidden) continue;
        if (str_ieq(platform_id, "favorites")) { if (!g->favorite) continue; }
        else if (str_ieq(platform_id, "recent")) { if (g->last_played <= 0) continue; }
        else if (!all && !str_ieq(g->platform_id, platform_id)) continue;
        if (filter == FILTER_FAVORITES && !g->favorite) continue;
        if (filter == FILTER_UNPLAYED && g->play_count > 0) continue;
        if (filter == FILTER_MULTIPLAYER && g->players <= 1) continue;
        if (query && *query) {
            if (!str_contains_icase(g->name, query) &&
                !str_contains_icase(g->genre, query) &&
                !str_contains_icase(g->developer, query) &&
                !str_contains_icase(g->publisher, query)) continue;
        }
        list.items = xrealloc(list.items, (size_t)(list.count + 1) * sizeof(Game *));
        list.items[list.count++] = g;
    }
    g_sort_mode = sort;
    g_filter_mode = filter;
    if (list.count > 1)
        qsort(list.items, (size_t)list.count, sizeof(Game *), cmp_sort);
    if (str_ieq(platform_id, "recent") && list.count > 1) {
        g_sort_mode = SORT_RECENT;
        qsort(list.items, (size_t)list.count, sizeof(Game *), cmp_sort);
    }
    return list;
}

void game_list_free(GameList *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

void library_note_play(Library *lib, Game *game, int64_t seconds)
{
    if (!game) return;
    game->play_count++;
    game->last_played = (int64_t)time(NULL);
    if (seconds > 0) game->play_seconds += seconds;
    library_save_cache(lib);
}

void library_toggle_favorite(Library *lib, Game *game)
{
    if (!game) return;
    game->favorite = !game->favorite;
    library_save_cache(lib);
}

int library_total_games(const Library *lib) { return lib->game_count; }

int library_favorites_count(const Library *lib)
{
    int n = 0;
    for (int i = 0; i < lib->game_count; i++) if (lib->games[i].favorite) n++;
    return n;
}

int64_t library_total_play_seconds(const Library *lib)
{
    int64_t total = 0;
    for (int i = 0; i < lib->game_count; i++) total += lib->games[i].play_seconds;
    return total;
}

/* ── Cache ────────────────────────────────────────────────────────────────── */

static char *cache_path(void)
{
    return xasprintf("%s/library.tsv", taiji_config_dir());
}

void library_save_cache(const Library *lib)
{
    char *path = cache_path();
    size_t cap = 8192, len = 0;
    char *buf = xmalloc(cap);
    buf[0] = '\0';
#define ADD(...) do { \
        int need = snprintf(NULL, 0, __VA_ARGS__); \
        while (len + (size_t)need + 1 > cap) { cap *= 2; buf = xrealloc(buf, cap); } \
        len += (size_t)snprintf(buf + len, cap - len, __VA_ARGS__); \
    } while (0)
    ADD("TAIJI-LIBRARY\t1\n");
    for (int i = 0; i < lib->platform_count; i++) {
        Platform *p = &lib->platforms[i];
        char *f[7] = {p->id, p->name, p->short_name, p->logo, p->root_dir, p->cover_path, NULL};
        char *esc[6];
        for (int k = 0; k < 6; k++) esc[k] = escape_field(f[k] ? f[k] : "");
        ADD("P\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n", esc[0], esc[1], esc[2], esc[3], esc[4], esc[5], p->game_count);
        for (int k = 0; k < 6; k++) free(esc[k]);
    }
    for (int i = 0; i < lib->game_count; i++) {
        Game *g = &lib->games[i];
        char *f[19] = {
            g->id, g->platform_id, g->name, g->sort_name, g->rom_path, g->cover_path,
            g->logo_path, g->description, g->developer, g->publisher, g->genre,
            g->release_date, g->command, NULL, NULL, NULL, NULL, NULL, NULL
        };
        char *esc[20];
        for (int k = 0; k < 13; k++) esc[k] = escape_field(f[k] ? f[k] : "");
        ADD("G\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%lld\t%d\t%lld\n",
            esc[0], esc[1], esc[2], esc[3], esc[4], esc[5], esc[6], esc[7], esc[8],
            esc[9], esc[10], esc[11], esc[12], g->players, g->favorite ? 1 : 0,
            g->hidden ? 1 : 0, (long long)g->last_played, g->play_count,
            (long long)g->play_seconds);
        for (int k = 0; k < 13; k++) free(esc[k]);
    }
#undef ADD
    write_file_atomic(path, buf, len);
    free(buf);
    free(path);
}

bool library_load_cache(Library *lib)
{
    char *path = cache_path();
    char *data = read_file(path, NULL);
    free(path);
    if (!data) return false;
    Library fresh;
    library_init(&fresh);
    char *save = NULL;
    bool header = false;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!header) { header = true; continue; }
        if (line[0] == 'P' && line[1] == '\t') {
            char *fields[16];
            int n = 0;
            char *p = line + 2;
            fields[n++] = p;
            while (*p && n < 16) {
                if (*p == '\t') { *p = '\0'; fields[n++] = p + 1; }
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (n >= 7) {
                char *vals[8];
                for (int i = 0; i < 7; i++) vals[i] = unescape_field(fields[i]);
                Platform *pl = platform_get(&fresh, vals[0], vals[1], vals[2], vals[3], vals[4]);
                free(pl->cover_path);
                pl->cover_path = *vals[5] ? xstrdup(vals[5]) : NULL;
                pl->game_count = atoi(vals[6]);
                for (int i = 0; i < 7; i++) free(vals[i]);
            }
        } else if (line[0] == 'G' && line[1] == '\t') {
            char *fields[32];
            int n = 0;
            char *p = line + 2;
            fields[n++] = p;
            while (*p && n < 32) {
                if (*p == '\t') { *p = '\0'; fields[n++] = p + 1; }
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (n >= 19) {
                Game *g = game_add(&fresh);
                char *v[20];
                for (int i = 0; i < 19; i++) v[i] = unescape_field(fields[i]);
                g->id = xstrdup(v[0]);
                g->platform_id = xstrdup(v[1]);
                g->name = xstrdup(v[2]);
                g->sort_name = xstrdup(v[3]);
                g->rom_path = xstrdup(v[4]);
                g->cover_path = *v[5] ? xstrdup(v[5]) : NULL;
                g->logo_path = *v[6] ? xstrdup(v[6]) : NULL;
                g->description = xstrdup(v[7]);
                g->developer = xstrdup(v[8]);
                g->publisher = xstrdup(v[9]);
                g->genre = xstrdup(v[10]);
                g->release_date = xstrdup(v[11]);
                g->command = xstrdup(v[12]);
                g->players = atoi(v[13]);
                g->favorite = atoi(v[14]) != 0;
                g->hidden = atoi(v[15]) != 0;
                g->last_played = atoll(v[16]);
                g->play_count = atoi(v[17]);
                g->play_seconds = atoll(v[18]);
                for (int i = 0; i < 19; i++) free(v[i]);
            }
        }
    }
    free(data);
    library_free(lib);
    *lib = fresh;
    return true;
}
