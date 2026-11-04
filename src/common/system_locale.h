// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_SYSTEM_LOCALE_H
#define TAIJI_SYSTEM_LOCALE_H

#include <stdbool.h>
#include <stddef.h>

bool system_timezone_get(char *out, size_t n);
bool system_timezone_set(const char *tz, char *error, size_t error_size);

/* 0 = 失败，1 = 仅太极和新启动应用生效，2 = 系统级已生效 */
int system_language_apply(int lang, char *error, size_t error_size);
void system_language_export(int lang);

#endif
