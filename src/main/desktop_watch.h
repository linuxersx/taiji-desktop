// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_DESKTOP_WATCH_H
#define TAIJI_DESKTOP_WATCH_H

#include <stdbool.h>

void desktop_watch_init(void);
void desktop_watch_close(void);
/* Returns true once after a debounced desktop-entry/package change. */
bool desktop_watch_poll(void);

#endif
