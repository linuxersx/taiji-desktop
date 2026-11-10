// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "theme_auto.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int wrap_min(int m)
{
    m %= 1440;
    if (m < 0) m += 1440;
    return m;
}

static int now_minutes(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_hour * 60 + lt.tm_min;
}

bool theme_auto_sun_times(int *sunrise_min, int *sunset_min)
{
    if (!sunrise_min || !sunset_min) return false;
    time_t now = time(NULL);
    struct tm lt;
    if (!localtime_r(&now, &lt)) return false;

    int doy = lt.tm_yday + 1;
    /* 默认坐标（上海）；可通过环境变量覆盖，不额外增加设置项。 */
    double lat = 31.2304;
    double lon = 121.4737;
    const char *ls = getenv("TAIJI_LATITUDE");
    const char *lo = getenv("TAIJI_LONGITUDE");
    if (ls && *ls) lat = atof(ls);
    if (lo && *lo) lon = atof(lo);
    if (lat > 89.8) lat = 89.8;
    if (lat < -89.8) lat = -89.8;

    double gamma = 2.0 * M_PI / 365.0 * (double)(doy - 1);
    double eqtime = 229.18 * (0.000075 +
                              0.001868 * cos(gamma) -
                              0.032077 * sin(gamma) -
                              0.014615 * cos(2 * gamma) -
                              0.040849 * sin(2 * gamma));
    double decl = 0.006918 -
                  0.399912 * cos(gamma) +
                  0.070257 * sin(gamma) -
                  0.006758 * cos(2 * gamma) +
                  0.000907 * sin(2 * gamma) -
                  0.002697 * cos(3 * gamma) +
                  0.00148 * sin(3 * gamma);

    double latr = lat * M_PI / 180.0;
    double cos_ha = cos(90.833 * M_PI / 180.0) /
                        (cos(latr) * cos(decl)) -
                    tan(latr) * tan(decl);
    if (cos_ha <= -1.0) {           /* 极昼 */
        *sunrise_min = 0;
        *sunset_min = 1439;
        return true;
    }
    if (cos_ha >= 1.0) {            /* 极夜 */
        *sunrise_min = 720;
        *sunset_min = 720;
        return true;
    }

    double ha = acos(cos_ha) * 180.0 / M_PI;
    double sunrise_utc = 720.0 - 4.0 * (lon + ha) - eqtime;
    double sunset_utc = 720.0 - 4.0 * (lon - ha) - eqtime;
    int offset = (int)(lt.tm_gmtoff / 60);
    *sunrise_min = wrap_min((int)lround(sunrise_utc) + offset);
    *sunset_min = wrap_min((int)lround(sunset_utc) + offset);
    return true;
}

bool theme_auto_is_dark(int dark_min, int light_min)
{
    int now = now_minutes();
    bool custom = dark_min >= 0 || light_min >= 0;

    if (custom) {
        int dark = dark_min >= 0 ? wrap_min(dark_min) : 20 * 60;
        int light = light_min >= 0 ? wrap_min(light_min) : 7 * 60;
        if (dark == light) return false;
        if (dark < light)
            return now >= dark && now < light;
        return now >= dark || now < light;
    }

    /* Default to the hand-held's local wall clock.  Sunrise/sunset depends on
     * a location, and many users leave the system clock in UTC; using Shanghai
     * coordinates with a UTC clock incorrectly turns daytime into night. */
    int dark = 19 * 60;
    int light = 7 * 60;
    return now >= dark || now < light;
}
