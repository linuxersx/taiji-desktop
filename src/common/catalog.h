// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_CATALOG_H
#define TAIJI_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *id;
    const char *name;
    const char *short_name;
    const char *logo;       /* file under assets/platform_logos */
    const char *aliases;    /* comma separated folder aliases */
    const char *extensions; /* comma separated, no dots */
    uint32_t accent;
} PlatformDef;

const PlatformDef *catalog_all(int *count);
const PlatformDef *catalog_by_id(const char *id);
const PlatformDef *catalog_by_folder(const char *folder_name);
const PlatformDef *catalog_by_extension(const char *ext);
bool catalog_accepts_extension(const PlatformDef *def, const char *ext);
bool catalog_is_image(const char *path);
bool catalog_is_rom(const char *path);
char *catalog_logo_path(const char *id);
char *catalog_default_command(const char *id, bool mac);
int catalog_core_candidates(const char *id, const char **out, int max);
const char *catalog_platform_name(const char *id);
const char *catalog_zh_name(const char *id);

#endif
