// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern StoreBackend store_pacman_backend;
extern StoreBackend store_flatpak_backend;

static StoreBackend *g_backends[] = {
    &store_pacman_backend,
    &store_flatpak_backend,
};

bool store_list_push(StorePackageList *list, const StorePackage *pkg)
{
    if (!list || !pkg) return false;
    if (list->count == list->capacity) {
        int cap = list->capacity ? list->capacity * 2 : 256;
        StorePackage *items = realloc(list->items,
                                      (size_t)cap * sizeof(StorePackage));
        if (!items) return false;
        list->items = items;
        list->capacity = cap;
    }
    list->items[list->count++] = *pkg;
    return true;
}

void store_list_free(StorePackageList *list)
{
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

StorePackage *store_list_find(StorePackageList *list, const char *source,
                              const char *id)
{
    if (!list || !source || !id) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].source, source) == 0 &&
            strcmp(list->items[i].id, id) == 0)
            return &list->items[i];
    }
    return NULL;
}

int store_backend_count(void)
{
    return (int)(sizeof(g_backends) / sizeof(g_backends[0]));
}

StoreBackend *store_backend_at(int index)
{
    if (index < 0 || index >= store_backend_count()) return NULL;
    return g_backends[index];
}

StoreBackend *store_backend_find(const char *source)
{
    if (!source) return NULL;
    for (int i = 0; i < store_backend_count(); i++) {
        if (strcmp(g_backends[i]->source, source) == 0) return g_backends[i];
    }
    return NULL;
}

const char *store_helper_path(void)
{
    const char *env = getenv("TAIJI_STORE_HELPER");
    if (env && *env) return env;
    return "/usr/libexec/taiji/taiji-store-helper";
}
