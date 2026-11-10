// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_LIBRETRO_H
#define TAIJI_LIBRETRO_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[64];
    char label[128];
    char path[512];
    bool compatible;
    bool recommended;
} LibretroCore;

/* All installed libretro cores, sorted by display label. */
int libretro_scan(LibretroCore *out, int max);

/* Installed cores that are known to support the platform, recommended first. */
int libretro_compatible(const char *platform_id, LibretroCore *out, int max);

/* Extract the path following -L/--libretro from a launch command. */
bool libretro_command_core(const char *command, char *path, size_t path_size);

/* Display label for a core file name (without _libretro.so). */
const char *libretro_label_for_name(const char *name, char *buf, size_t buf_size);

/* Drop the inventory cache, e.g. after installing cores. */
void libretro_refresh(void);

#endif
