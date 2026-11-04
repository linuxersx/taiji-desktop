// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "appstream_index.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static bool has_suffix_ci(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcasecmp(s + sl - fl, suffix) == 0;
}

static void xml_value(const char *line, const char *tag, char *out, size_t n)
{
    const char *p = strstr(line, tag);
    if (!p) return;
    p += strlen(tag);
    const char *end = strstr(p, "</");
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static AppStreamEntry *entry_for(AppStreamIndex *idx, const char *pkg)
{
    for (int i = 0; i < idx->count; i++)
        if (strcmp(idx->items[i].pkg, pkg) == 0) return &idx->items[i];
    if (idx->count == idx->cap) {
        int cap = idx->cap ? idx->cap * 2 : 256;
        idx->items = realloc(idx->items, (size_t)cap * sizeof(AppStreamEntry));
        idx->cap = cap;
    }
    AppStreamEntry *e = &idx->items[idx->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->pkg, sizeof(e->pkg), "%s", pkg);
    return e;
}

static void parse_file(AppStreamIndex *idx, const char *path)
{
    gzFile f = gzopen(path, "rb");
    if (!f) return;
    char line[8192];
    bool in_component = false;
    bool desktop = false;
    char pkgs[16][128];
    int pkg_count = 0;
    AppStreamEntry cur;
    memset(&cur, 0, sizeof(cur));
    while (gzgets(f, line, sizeof(line))) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strstr(line, "<component")) {
            in_component = true;
            desktop = strstr(line, "desktop-application") != NULL ||
                      strstr(line, "desktop") != NULL;
            memset(&cur, 0, sizeof(cur));
            pkg_count = 0;
            continue;
        }
        if (strstr(line, "</component>")) {
            if (in_component && desktop && cur.icon[0]) {
                for (int i = 0; i < pkg_count; i++) {
                    AppStreamEntry *e = entry_for(idx, pkgs[i]);
                    snprintf(e->icon, sizeof(e->icon), "%s", cur.icon);
                    if (cur.name[0]) snprintf(e->name, sizeof(e->name), "%s", cur.name);
                    if (cur.summary[0]) snprintf(e->summary, sizeof(e->summary), "%s", cur.summary);
                    if (cur.categories[0]) snprintf(e->categories, sizeof(e->categories), "%s", cur.categories);
                }
            }
            in_component = false;
            continue;
        }
        if (!in_component) continue;
        if (strstr(line, "<pkgname>")) {
            char pkg[128];
            xml_value(line, "<pkgname>", pkg, sizeof(pkg));
            if (pkg[0] && pkg_count < 16)
                snprintf(pkgs[pkg_count++], sizeof(pkgs[0]), "%s", pkg);
        } else if (strstr(line, "<icon type=\"stock\">")) {
            xml_value(line, "<icon type=\"stock\">", cur.icon, sizeof(cur.icon));
        } else if (strstr(line, "<name>")) {
            char tmp[160];
            xml_value(line, "<name>", tmp, sizeof(tmp));
            if (!cur.name[0]) snprintf(cur.name, sizeof(cur.name), "%s", tmp);
        } else if (strstr(line, "<summary>")) {
            char tmp[256];
            xml_value(line, "<summary>", tmp, sizeof(tmp));
            if (!cur.summary[0]) snprintf(cur.summary, sizeof(cur.summary), "%s", tmp);
        } else if (strstr(line, "<categories>")) {
            xml_value(line, "<categories>", cur.categories, sizeof(cur.categories));
        }
    }
    gzclose(f);
}

void appstream_index_init(AppStreamIndex *idx)
{
    memset(idx, 0, sizeof(*idx));
}

void appstream_index_free(AppStreamIndex *idx)
{
    if (!idx) return;
    free(idx->items);
    memset(idx, 0, sizeof(*idx));
}

bool appstream_index_build(AppStreamIndex *idx)
{
    if (!idx) return false;
    const char *env = getenv("TAIJI_APPSTREAM_DIR");
    const char *dirs[3];
    int n = 0;
    if (env && *env) dirs[n++] = env;
    else {
        dirs[n++] = "/usr/share/swcatalog/xml";
        dirs[n++] = "/usr/share/app-info/xmls";
    }
    for (int d = 0; d < n; d++) {
        int count = 0;
        char **items = list_dir(dirs[d], &count);
        if (!items) continue;
        for (int i = 0; i < count; i++) {
            if (!has_suffix_ci(items[i], ".xml.gz")) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], items[i]);
            parse_file(idx, path);
        }
        free_strv(items, count);
    }
    return idx->count > 0;
}

const AppStreamEntry *appstream_index_find(const AppStreamIndex *idx, const char *pkg)
{
    if (!idx || !pkg) return NULL;
    for (int i = 0; i < idx->count; i++)
        if (strcasecmp(idx->items[i].pkg, pkg) == 0) return &idx->items[i];
    return NULL;
}

const char *appstream_index_icon(const AppStreamIndex *idx, const char *pkg)
{
    const AppStreamEntry *e = appstream_index_find(idx, pkg);
    return e && e->icon[0] ? e->icon : NULL;
}
