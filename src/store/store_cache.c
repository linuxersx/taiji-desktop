// SPDX-License-Identifier: GPL-3.0-or-later
#include "store_cache.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void clean_field(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
}

char *store_cache_path(const char *source)
{
    const char *cache = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char dir[PATH_MAX];
    if (cache && *cache)
        snprintf(dir, sizeof(dir), "%s/taiji/store", cache);
    else
        snprintf(dir, sizeof(dir), "%s/.cache/taiji/store", home ? home : "/tmp");
    mkdir_p(dir);
    char *path = xmalloc(strlen(dir) + strlen(source) + 8);
    sprintf(path, "%s/%s.tsv", dir, source);
    return path;
}

bool store_cache_save(const char *source, const StorePackageList *list)
{
    if (!source || !list) return false;
    char *path = store_cache_path(source);
    FILE *f = fopen(path, "w");
    free(path);
    if (!f) return false;
    fprintf(f, "source\tid\tname\tversion\tinstalled_version\tdescription\t"
               "license\tcategory\ticon\tsize_kb\tinstalled\tupdate\tremovable\n");
    for (int i = 0; i < list->count; i++) {
        const StorePackage *p = &list->items[i];
        char name[STORE_NAME_MAX], desc[STORE_TEXT_MAX], icon[512];
        snprintf(name, sizeof(name), "%s", p->name);
        snprintf(desc, sizeof(desc), "%s", p->description);
        snprintf(icon, sizeof(icon), "%s", p->icon);
        clean_field(name);
        clean_field(desc);
        clean_field(icon);
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%lld\t%d\t%d\t%d\n",
                p->source, p->id, name, p->version, p->installed_version,
                desc, p->license, p->category, icon, p->size_kb,
                p->installed ? 1 : 0, p->update_available ? 1 : 0,
                p->removable ? 1 : 0);
    }
    fclose(f);
    return true;
}

bool store_cache_load(const char *source, StorePackageList *out)
{
    if (!source || !out) return false;
    char *path = store_cache_path(source);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return false;
    char line[4096];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (first) { first = false; continue; }
        if (!line[0]) continue;
        char *fields[13] = {0};
        int n = 0;
        char *p = line;
        while (n < 13) {
            fields[n++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n < 13) continue;
        StorePackage pkg;
        memset(&pkg, 0, sizeof(pkg));
        snprintf(pkg.source, sizeof(pkg.source), "%s", fields[0]);
        snprintf(pkg.id, sizeof(pkg.id), "%s", fields[1]);
        snprintf(pkg.name, sizeof(pkg.name), "%s", fields[2]);
        snprintf(pkg.version, sizeof(pkg.version), "%s", fields[3]);
        snprintf(pkg.installed_version, sizeof(pkg.installed_version), "%s", fields[4]);
        snprintf(pkg.description, sizeof(pkg.description), "%s", fields[5]);
        snprintf(pkg.license, sizeof(pkg.license), "%s", fields[6]);
        snprintf(pkg.category, sizeof(pkg.category), "%s", fields[7]);
        snprintf(pkg.icon, sizeof(pkg.icon), "%s", fields[8]);
        pkg.size_kb = atoll(fields[9]);
        pkg.installed = atoi(fields[10]) != 0;
        pkg.update_available = atoi(fields[11]) != 0;
        pkg.removable = atoi(fields[12]) != 0;
        store_list_push(out, &pkg);
    }
    fclose(f);
    return true;
}
