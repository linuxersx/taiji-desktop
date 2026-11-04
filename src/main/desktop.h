// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_DESKTOP_H
#define TAIJI_DESKTOP_H

#include <stddef.h>

#include "apps.h"

typedef struct {
    int added;
    int updated;
    int skipped;
    int removed;
} DesktopImportStats;

/* Import .desktop files from standard Linux application directories.
 * Returns false only when the scan itself could not run. */
DesktopImportStats desktop_import_apps(AppList *list);
DesktopImportStats desktop_import_auto(AppList *list);
void desktop_mark_imported(void);
bool desktop_import_done(void);

/* Convert an icon (SVG/XPM or raster) into something LVGL can load.
 * SVG/XPM are rasterized into the icon cache and the PNG path is returned.
 * Returns false when the file is missing or cannot be converted. */
bool desktop_icon_to_png(const char *icon, char *out, size_t out_size);
bool desktop_icon_cached_png(const char *icon, char *out, size_t out_size);

#endif
