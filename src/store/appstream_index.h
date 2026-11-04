// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_APPSTREAM_INDEX_H
#define TAIJI_APPSTREAM_INDEX_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char pkg[128];
    char icon[160];
    char name[160];
    char summary[256];
    char categories[256];
} AppStreamEntry;

typedef struct {
    AppStreamEntry *items;
    int count;
    int cap;
} AppStreamIndex;

void appstream_index_init(AppStreamIndex *idx);
void appstream_index_free(AppStreamIndex *idx);
bool appstream_index_build(AppStreamIndex *idx);

/* 返回包名对应的 stock 图标名，没有则返回 NULL。 */
const char *appstream_index_icon(const AppStreamIndex *idx, const char *pkg);
const AppStreamEntry *appstream_index_find(const AppStreamIndex *idx, const char *pkg);

#endif
