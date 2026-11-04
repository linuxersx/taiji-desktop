// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "system_status.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#ifdef __linux__
static int read_int_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void read_text_file(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        char *p = str_trim(out);
        if (p != out) memmove(out, p, strlen(p) + 1);
    }
    fclose(f);
}
#endif

#ifdef __linux__
static void scan_linux_wifi(SystemStatus *out)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    int found = 0;
    int connected = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strncmp(e->d_name, "wlan", 4) != 0 &&
            strncmp(e->d_name, "wlp", 3) != 0 &&
            strncmp(e->d_name, "wl", 2) != 0) {
            continue;
        }
        char *base = xasprintf("/sys/class/net/%s", e->d_name);
        char *wireless = path_join(base, "wireless");
        char *state = path_join(base, "operstate");
        bool is_wifi = dir_exists(wireless) || strncmp(e->d_name, "wlan", 4) == 0 ||
                       strncmp(e->d_name, "wlp", 3) == 0;
        char st[32];
        read_text_file(state, st, sizeof(st));
        if (is_wifi) {
            found = 1;
            if (str_ieq(st, "up")) connected = 1;
            if (!connected) {
                FILE *f = fopen("/proc/net/wireless", "r");
                if (f) {
                    char line[512];
                    while (fgets(line, sizeof(line), f)) {
                        if (strstr(line, e->d_name) && strchr(line, ':')) {
                            char *colon = strchr(line, ':');
                            int q = 0;
                            if (colon && sscanf(colon + 1, "%d", &q) == 1 && q > 0)
                                connected = 1;
                        }
                    }
                    fclose(f);
                }
            }
        }
        free(base);
        free(wireless);
        free(state);
    }
    closedir(d);
    if (found) out->wifi = connected ? 1 : 0;
}
#endif

#ifdef __APPLE__
static void scan_macos_wifi(SystemStatus *out)
{
    const char *ifaces[] = {"en0", "en1", NULL};
    for (int i = 0; ifaces[i]; i++) {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "networksetup -getairportnetwork %s 2>/dev/null", ifaces[i]);
        FILE *f = popen(cmd, "r");
        if (!f) continue;
        char line[512];
        bool known = false;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "Current Wi-Fi Network:")) {
                out->wifi = 1;
                known = true;
                break;
            }
            if (strstr(line, "not associated")) {
                out->wifi = 0;
                known = true;
                break;
            }
        }
        pclose(f);
        if (known) return;
    }
}
#endif

#ifdef __linux__
static void scan_battery_sysfs(SystemStatus *out)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *base = xasprintf("/sys/class/power_supply/%s", e->d_name);
        char *type_path = path_join(base, "type");
        char type[32];
        read_text_file(type_path, type, sizeof(type));
        if (!str_ieq(type, "Battery")) {
            free(base); free(type_path);
            continue;
        }
        char *cap_path = path_join(base, "capacity");
        char *status_path = path_join(base, "status");
        int cap = read_int_file(cap_path);
        char status[32];
        read_text_file(status_path, status, sizeof(status));
        if (cap >= 0) out->battery = cap;
        out->charging = str_ieq(status, "Charging") || str_ieq(status, "Full");

        char *ef = path_join(base, "energy_full");
        char *ed = path_join(base, "energy_full_design");
        int full = read_int_file(ef);
        int design = read_int_file(ed);
        free(ef); free(ed);
        if (full <= 0 || design <= 0) {
            char *cf = path_join(base, "charge_full");
            char *cd = path_join(base, "charge_full_design");
            full = read_int_file(cf);
            design = read_int_file(cd);
            free(cf); free(cd);
        }
        if (full > 0 && design > 0) {
            int health = (int)(((long long)full * 100 + design / 2) / design);
            if (health < 0) health = 0;
            if (health > 100) health = 100;
            out->battery_health = health;
        }
        char *hp = path_join(base, "health");
        read_text_file(hp, out->battery_health_label,
                       sizeof(out->battery_health_label));
        free(hp);
        free(base); free(type_path); free(cap_path); free(status_path);
        break;
    }
    closedir(d);
}
#endif

#ifdef __APPLE__
static int macos_battery_health(void)
{
    static bool scanned = false;
    static int cached = -1;
    if (scanned) return cached;
    scanned = true;

    FILE *f = popen("ioreg -rn AppleSmartBattery 2>/dev/null", "r");
    if (!f) return cached;
    int design = 0, raw_max = 0, max_cap = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *p;
        int v = 0;
        if ((p = strstr(line, "\"DesignCapacity\"")) &&
            sscanf(p, "\"DesignCapacity\" = %d", &v) == 1)
            design = v;
        else if ((p = strstr(line, "\"AppleRawMaxCapacity\"")) &&
                 sscanf(p, "\"AppleRawMaxCapacity\" = %d", &v) == 1)
            raw_max = v;
        else if ((p = strstr(line, "\"MaxCapacity\"")) &&
                 sscanf(p, "\"MaxCapacity\" = %d", &v) == 1)
            max_cap = v;
    }
    pclose(f);

    if (design > 0 && raw_max > 0) {
        int health = (int)(((long long)raw_max * 100 + design / 2) / design);
        cached = health > 100 ? 100 : health;
    } else if (max_cap > 0 && max_cap <= 100) {
        cached = max_cap;
    } else if (design > 0 && max_cap > 0) {
        int health = (int)(((long long)max_cap * 100 + design / 2) / design);
        cached = health > 100 ? 100 : health;
    }
    return cached;
}
#endif

void system_status_poll(SystemStatus *out)
{
    if (!out) return;
    out->wifi = -1;
    out->battery = -1;
    out->charging = false;
    out->battery_health = -1;
    out->battery_health_label[0] = '\0';

#ifdef __linux__
    scan_linux_wifi(out);
#endif
#ifdef __APPLE__
    scan_macos_wifi(out);
#endif

    int secs = -1, pct = -1;
    SDL_PowerState ps = SDL_GetPowerInfo(&secs, &pct);
    if (ps == SDL_POWERSTATE_NO_BATTERY || ps == SDL_POWERSTATE_UNKNOWN) {
        out->battery = -1;
        out->charging = false;
    } else if (pct >= 0 && pct <= 100) {
        out->battery = pct;
        out->charging = ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED;
    }
#ifdef __linux__
    scan_battery_sysfs(out);
#endif
#ifdef __APPLE__
    out->battery_health = macos_battery_health();
#endif
}
