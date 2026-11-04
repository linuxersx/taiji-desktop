// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_MIRROR_H
#define TAIJI_MIRROR_H

#include <stdbool.h>
#include <stddef.h>

#include "store.h"

int store_mirror_count(void);
const char *store_mirror_label(int index);
const char *store_mirror_url(int index);
const char *store_mirror_id(int index);

/* Index of the currently configured mirror, or -1 when unknown. */
int store_mirror_current(void);
const char *store_mirror_path(void);

/* Ask the privileged helper to rewrite the mirror list and refresh pacman. */
bool store_mirror_apply(int index, StoreProgress *progress,
                        char *error, size_t error_size);

#endif
