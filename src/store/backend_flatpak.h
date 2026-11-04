// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_BACKEND_FLATPAK_H
#define TAIJI_BACKEND_FLATPAK_H

#include "store.h"

extern StoreBackend store_flatpak_backend;

int store_flatpak_mirror_count(void);
const char *store_flatpak_mirror_label(int index);
const char *store_flatpak_mirror_url(int index);
const char *store_flatpak_mirror_id(int index);
int store_flatpak_mirror_current(void);

bool store_flatpak_mirror_apply(int index, StoreProgress *progress,
                                char *error, size_t error_size);

#endif
