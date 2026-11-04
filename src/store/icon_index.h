// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_ICON_INDEX_H
#define TAIJI_ICON_INDEX_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[160];
    char path[512];
    int rank;
} IconEntry;

typedef struct {
    char key[160];
    char icon[160];
} IconDesktop;

typedef struct {
    IconEntry *icons;
    int icon_count;
    int icon_cap;
    int *slots;
    int slot_cap;
    IconDesktop *desktops;
    int desktop_count;
    int desktop_cap;
} IconIndex;

void icon_index_init(IconIndex *idx);
void icon_index_free(IconIndex *idx);
bool icon_index_build(IconIndex *idx);

/* 解析包名到图标绝对路径。成功返回 true。 */
bool icon_index_lookup_name(const IconIndex *idx, const char *name,
                            char *out, size_t out_size);
bool icon_index_resolve(const IconIndex *idx, const char *pkg,
                        char *out, size_t out_size);

#endif
