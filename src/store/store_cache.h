// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_STORE_CACHE_H
#define TAIJI_STORE_CACHE_H

#include "store.h"

bool store_cache_load(const char *source, StorePackageList *out);
bool store_cache_save(const char *source, const StorePackageList *list);
char *store_cache_path(const char *source);

#endif
