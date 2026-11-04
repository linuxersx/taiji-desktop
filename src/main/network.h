// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TAIJI_NETWORK_H
#define TAIJI_NETWORK_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char ssid[128];
    char security[64];
    int signal;      /* 0..100 */
    bool active;
    bool secure;
} WifiNetwork;

typedef struct {
    WifiNetwork items[64];
    int count;
    bool enabled;
    bool available;
    char current_ssid[128];
    int current_signal;
    char message[256];
} WifiState;

void network_wifi_scan(WifiState *state, bool rescan);
void network_wifi_scan_async(bool rescan);
bool network_wifi_scan_take(WifiState *state);
bool network_wifi_radio(bool *enabled, bool *available);
bool network_wifi_toggle(bool enabled, char *error, size_t error_size);
bool network_wifi_connect(const char *ssid, const char *password,
                          char *error, size_t error_size);

#endif
