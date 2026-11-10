// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SYSTEM_BRIGHTNESS_H
#define TAIJI_SYSTEM_BRIGHTNESS_H

#include <stdbool.h>

/* Returns 1..100, or -1 when no writable backlight is available. */
int system_brightness_get(void);
bool system_brightness_set(int percent);
bool system_brightness_available(void);

#endif
