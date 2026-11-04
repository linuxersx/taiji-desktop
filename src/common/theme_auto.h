// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_THEME_AUTO_H
#define TAIJI_THEME_AUTO_H

#include <stdbool.h>

/* 返回本地时间下的日出、日落分钟数。 */
bool theme_auto_sun_times(int *sunrise_min, int *sunset_min);

/* 自定义时间任一有效时优先使用自定义时间，否则按日出/日落判断。 */
bool theme_auto_is_dark(int dark_min, int light_min);

#endif
